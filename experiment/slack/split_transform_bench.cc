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

// Build the shared input buffer with JSON records
void build_input_buffer_json(uint32_t num_fields, uint32_t field_length) {
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
            
            char prefix[64];
            if (f == 0) {
                snprintf(prefix, sizeof(prefix), "{\"field_%u\":\"", f + 1);
            } else {
                snprintf(prefix, sizeof(prefix), "\"field_%u\":\"", f + 1);
            }
            int prefix_len = strlen(prefix);
            
            int padding = field_length - prefix_len - ((f == num_fields - 1) ? 2 : 2); // 2 for "\", or "\"}\n"
            if (padding < 1) padding = 1;

            memcpy(fp, prefix, prefix_len);
            memset(fp + prefix_len, 'A' + (f % 26), padding);
            
            if (f == num_fields - 1) {
                fp[prefix_len + padding] = '"';
                fp[prefix_len + padding + 1] = '}';
                for(uint32_t i = prefix_len + padding + 2; i < field_length - 1; i++) fp[i] = ' ';
                fp[field_length - 1] = '\n';
            } else {
                fp[prefix_len + padding] = '"';
                fp[prefix_len + padding + 1] = ',';
                for(uint32_t i = prefix_len + padding + 2; i < field_length; i++) fp[i] = ' ';
            }
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
    // Allocate arrays for the two-pass parse-then-split
    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t* field_lengths = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        // Ensure we don't overflow the ring buffers
        for (uint32_t x = 0; x < a->x_ways; x++) {
            if (ring_offsets[x] + record_size >= RING_BUFFER_SIZE) {
                ring_offsets[x] = 0; // Wrap around
            }
        }

        // 1. Parse one record into fields
        uint32_t parsed_fields = 0;
        uint32_t src_pos = 0;
        uint32_t current_field_start = 0;

        while (src_pos < record_size && parsed_fields < a->num_fields) {
            char c = src[src_pos];
            if (c == '|' || c == ',' || c == '\n') {
                field_ptrs[parsed_fields] = src + current_field_start;
                field_lengths[parsed_fields] = src_pos - current_field_start;
                parsed_fields++;
                current_field_start = src_pos + 1;
            }
            src_pos++;
        }
        // Handle last field if it didn't end with a delimiter
        if (parsed_fields < a->num_fields && current_field_start < record_size) {
            field_ptrs[parsed_fields] = src + current_field_start;
            field_lengths[parsed_fields] = record_size - current_field_start;
            parsed_fields++;
        }

        // 2. Split the parsed fields into X ways
        uint32_t field_idx = 0;
        for (uint32_t x = 0; x < a->x_ways; x++) {
            uint32_t target_fields = way_limits[x];
            char* dest = ring_buffers[x] + ring_offsets[x];
            uint32_t dest_offset = 0;

            while (field_idx < target_fields && field_idx < parsed_fields) {
                // Copy the field payload
                memcpy(dest + dest_offset, field_ptrs[field_idx], field_lengths[field_idx]);
                dest_offset += field_lengths[field_idx];

                // Add delimiter or newline
                if (field_idx == target_fields - 1 || field_idx == parsed_fields - 1) {
                    dest[dest_offset++] = '\n';
                } else {
                    dest[dest_offset++] = '|';
                }
                field_idx++;
            }
            ring_offsets[x] += dest_offset;
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
    free(field_ptrs);
    free(field_lengths);
    free(way_limits);

    return NULL;
}

static void* worker_json(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;
    
    // Allocate X ring buffers for this thread
    char** ring_buffers = (char**)malloc(a->x_ways * sizeof(char*));
    uint64_t* ring_offsets = (uint64_t*)calloc(a->x_ways, sizeof(uint64_t));
    for (uint32_t x = 0; x < a->x_ways; x++) {
        ring_buffers[x] = (char*)malloc(RING_BUFFER_SIZE);
    }

    uint32_t* way_limits = (uint32_t*)malloc(a->x_ways * sizeof(uint32_t));
    for (uint32_t x = 0; x < a->x_ways; x++) {
        way_limits[x] = ((x + 1) * a->num_fields) / a->x_ways;
    }

    uint64_t count = 0;
    uint8_t sink = 0;
    
    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;
    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t* field_lengths = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        for (uint32_t x = 0; x < a->x_ways; x++) {
            if (ring_offsets[x] + record_size >= RING_BUFFER_SIZE) {
                ring_offsets[x] = 0; // Wrap around
            }
        }

        // 1. Parse JSON record into fields
        uint32_t parsed_fields = 0;
        uint32_t src_pos = 0;
        
        // Skip opening brace if it exists
        if (src[src_pos] == '{') src_pos++;
        
        uint32_t current_field_start = src_pos;
        bool in_quotes = false;

        while (src_pos < record_size && parsed_fields < a->num_fields) {
            char c = src[src_pos];
            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (!in_quotes && (c == ',' || c == '}')) {
                field_ptrs[parsed_fields] = src + current_field_start;
                field_lengths[parsed_fields] = src_pos - current_field_start;
                parsed_fields++;
                
                src_pos++;
                while(src_pos < record_size && src[src_pos] == ' ') src_pos++;
                current_field_start = src_pos;
                continue;
            }
            src_pos++;
        }
        if (parsed_fields < a->num_fields && current_field_start < record_size) {
            field_ptrs[parsed_fields] = src + current_field_start;
            field_lengths[parsed_fields] = record_size - current_field_start;
            parsed_fields++;
        }

        // 2. Split the parsed fields into X ways (Valid JSON documents)
        uint32_t field_idx = 0;
        for (uint32_t x = 0; x < a->x_ways; x++) {
            uint32_t target_fields = way_limits[x];
            char* dest = ring_buffers[x] + ring_offsets[x];
            uint32_t dest_offset = 0;

            dest[dest_offset++] = '{';

            while (field_idx < target_fields && field_idx < parsed_fields) {
                memcpy(dest + dest_offset, field_ptrs[field_idx], field_lengths[field_idx]);
                dest_offset += field_lengths[field_idx];

                if (field_idx == target_fields - 1 || field_idx == parsed_fields - 1) {
                    dest[dest_offset++] = '}';
                    dest[dest_offset++] = '\n';
                } else {
                    dest[dest_offset++] = ',';
                }
                field_idx++;
            }
            ring_offsets[x] += dest_offset;
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
    free(field_ptrs);
    free(field_lengths);
    free(way_limits);

    return NULL;
}

int main(int argc, const char** argv) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: ./split_transform_bench <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]\n"
            "Example: ./split_transform_bench 10 4 16 128 2 json\n"
        );
        return 1;
    }

    int duration_s = atoi(argv[1]);
    uint32_t n_workers = atoi(argv[2]);
    uint32_t num_fields = atoi(argv[3]);
    uint32_t field_length = atoi(argv[4]);
    uint32_t x_ways = atoi(argv[5]);
    
    bool is_json = false;
    if (argc >= 7 && strcmp(argv[6], "json") == 0) {
        is_json = true;
    }

    if (duration_s <= 0 || n_workers == 0 || num_fields == 0 || field_length == 0 || x_ways == 0) {
        fprintf(stderr, "All parameters must be > 0\n");
        return 1;
    }
    if (x_ways > num_fields) {
        fprintf(stderr, "x_ways cannot be greater than num_fields\n");
        return 1;
    }

    if (is_json) {
        printf("split_transform_bench: building 1 GiB input buffer (JSON format)...\n");
        build_input_buffer_json(num_fields, field_length);
    } else {
        printf("split_transform_bench: building 1 GiB input buffer (CSV format)...\n");
        build_input_buffer(num_fields, field_length);
    }

    printf("split_transform_bench: %u worker(s) × %ds\n"
           "  Record: %u fields × %u B = %u B total\n"
           "  Split:  %u ways\n"
           "  Format: %s\n",
           n_workers, duration_s, num_fields, field_length, num_fields * field_length, x_ways, is_json ? "JSON" : "CSV");

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
        if (is_json) {
            pthread_create(&threads[i], NULL, worker_json, &args[i]);
        } else {
            pthread_create(&threads[i], NULL, worker, &args[i]);
        }
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
