/**
 * split_transform_bench.cc — Out-of-cache CSV column-split benchmark
 *
 * Simulates a split transformation on data that does NOT fit in CPU cache.
 * 
 * Features:
 * - Pregenerates a massive shared input buffer of CSV records (e.g., 1 GiB) to
 *   defeat L1/L2/L3 caching, forcing real memory bandwidth utilization during parsing.
 * - Splits records of (NUM_FIELDS * FIELD_LENGTH) into X_WAYS.
 * - Parses the CSV (finding delimiters) and writes the split partitions into
 *   X independent ring buffers per thread.
 *
 * Usage:
 *   ./split_transform_bench <duration_s> <n_workers> <num_fields> <field_length> <x_ways>
 *
 * Compile:
 *   g++ -O3 -pthread -o split_transform_bench split_transform_bench.cc
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>

#define NS_PER_S 1000000000ULL
#define INPUT_BUFFER_SIZE (1024ULL * 1024ULL * 1024ULL) // 1 GiB shared input to defeat L3
#define RING_BUFFER_SIZE  (16ULL * 1024ULL * 1024ULL)   // 16 MiB per ring buffer per thread

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

char* shared_input_buffer = NULL;
uint64_t actual_input_size = 0;

typedef struct {
    int tid;
    uint64_t end_ns;
    uint32_t num_fields;
    uint32_t field_length;
    uint32_t x_ways;
    uint64_t count;
    uint8_t  sink;
} WorkerArgs;

// Build the shared input buffer with CSV records
void build_input_buffer(uint32_t num_fields, uint32_t field_length) {
    shared_input_buffer = (char*)malloc(INPUT_BUFFER_SIZE);
    if (!shared_input_buffer) {
        fprintf(stderr, "Failed to allocate 1 GiB input buffer.\n");
        exit(1);
    }

    uint32_t record_size = num_fields * field_length;
    actual_input_size = (INPUT_BUFFER_SIZE / record_size) * record_size;

    for (uint64_t offset = 0; offset < actual_input_size; offset += record_size) {
        char *rec = shared_input_buffer + offset;
        for (uint32_t f = 0; f < num_fields; f++) {
            char *fp = rec + f * field_length;
            // Fill field with dummy data
            memset(fp, 'A' + (f % 26), field_length);
            // Put delimiter at the end of the field (except last field gets newline)
            fp[field_length - 1] = (f == num_fields - 1) ? '\n' : '|';
        }
    }
}

static void* worker(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;
    
    // Allocate X ring buffers for this thread
    char** ring_buffers = (char**)malloc(a->x_ways * sizeof(char*));
    uint64_t* ring_offsets = (uint64_t*)calloc(a->x_ways, sizeof(uint64_t));
    for (uint32_t x = 0; x < a->x_ways; x++) {
        ring_buffers[x] = (char*)malloc(RING_BUFFER_SIZE);
    }

    // Determine how many fields go to each way
    uint32_t* way_limits = (uint32_t*)malloc(a->x_ways * sizeof(uint32_t));
    for (uint32_t x = 0; x < a->x_ways; x++) {
        way_limits[x] = ((x + 1) * a->num_fields) / a->x_ways;
    }

    uint64_t count = 0;
    uint8_t sink = 0;
    
    // Start at a staggered offset to prevent all threads reading the exact same cache line
    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;
        
        uint32_t current_way = 0;
        uint32_t field_idx = 0;
        
        // Ensure we don't overflow the ring buffers
        for (uint32_t x = 0; x < a->x_ways; x++) {
            if (ring_offsets[x] + record_size >= RING_BUFFER_SIZE) {
                ring_offsets[x] = 0; // Wrap around
            }
        }

        // Parse one record
        uint32_t src_pos = 0;
        while (src_pos < record_size) {
            char c = src[src_pos++];
            
            // Write to the current ring buffer
            char* dest = ring_buffers[current_way] + ring_offsets[current_way];
            *dest = c;
            ring_offsets[current_way]++;

            if (c == '|' || c == '\n') {
                field_idx++;
                if (field_idx >= way_limits[current_way]) {
                    // We finished the fields for the current way.
                    // Replace the last delimiter with a newline for the sub-record
                    *dest = '\n';
                    current_way++;
                    if (current_way >= a->x_ways) {
                        break;
                    }
                }
            }
        }

        sink ^= ring_buffers[0][0]; // Anti-elide
        count++;

        read_offset += record_size;
        if (read_offset >= actual_input_size) {
            read_offset = 0;
        }
    }

    a->count = count;
    a->sink = sink;

    for (uint32_t x = 0; x < a->x_ways; x++) {
        free(ring_buffers[x]);
    }
    free(ring_buffers);
    free(ring_offsets);
    free(way_limits);

    return NULL;
}

int main(int argc, const char** argv) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: ./split_transform_bench <duration_s> <n_workers> <num_fields> <field_length> <x_ways>\n"
            "Example: ./split_transform_bench 10 4 16 128 2\n"
        );
        return 1;
    }

    int duration_s = atoi(argv[1]);
    uint32_t n_workers = atoi(argv[2]);
    uint32_t num_fields = atoi(argv[3]);
    uint32_t field_length = atoi(argv[4]);
    uint32_t x_ways = atoi(argv[5]);

    if (duration_s <= 0 || n_workers == 0 || num_fields == 0 || field_length == 0 || x_ways == 0) {
        fprintf(stderr, "All parameters must be > 0\n");
        return 1;
    }
    if (x_ways > num_fields) {
        fprintf(stderr, "x_ways cannot be greater than num_fields\n");
        return 1;
    }

    printf("split_transform_bench: building 1 GiB input buffer (defeating cache)...\n");
    build_input_buffer(num_fields, field_length);

    printf("split_transform_bench: %u worker(s) × %ds\n"
           "  Record: %u fields × %u B = %u B total\n"
           "  Split:  %u ways\n",
           n_workers, duration_s, num_fields, field_length, num_fields * field_length, x_ways);

    uint64_t end_ns = get_ns() + (uint64_t)duration_s * NS_PER_S;

    WorkerArgs* args = (WorkerArgs*)calloc(n_workers, sizeof(WorkerArgs));
    pthread_t* threads = (pthread_t*)calloc(n_workers, sizeof(pthread_t));

    for (uint32_t i = 0; i < n_workers; i++) {
        args[i].tid = i;
        args[i].end_ns = end_ns;
        args[i].num_fields = num_fields;
        args[i].field_length = field_length;
        args[i].x_ways = x_ways;
        args[i].count = 0;
        args[i].sink = 0;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (uint32_t i = 0; i < n_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t total_splits = 0;
    uint8_t sink_accum = 0;
    for (uint32_t i = 0; i < n_workers; i++) {
        total_splits += args[i].count;
        sink_accum ^= args[i].sink;
    }
    (void)sink_accum;

    free(args);
    free(threads);
    free(shared_input_buffer);

    double rate = (double)total_splits / (double)duration_s;
    printf("split_transform_bench: workers=%u  splits=%lu  rate=%.0f splits/s\n",
           n_workers, (unsigned long)total_splits, rate);

    return 0;
}
