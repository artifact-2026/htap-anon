/**
 * augment_transform_bench.cc — Out-of-cache secondary index build benchmark
 *
 * Simulates the "augment" transform: scanning a base table and building a
 * secondary index over one or more selected fields.  The indexed fields are
 * specified by their 0-based position in the record on the command line.
 *
 * Index modes:
 *   hash — Open-addressing hash table keyed on the composite field key (FNV-1a).
 *           Models the cost of hash-index construction: random write access into
 *           a large-ish index structure, plus collision-probe overhead.
 *           Each thread owns a private table (HASH_TABLE_SIZE slots); the table
 *           is reset when 75% full to simulate periodic flush/compaction.
 *
 *   sort — Batch sort-merge index build: (composite-key, record-id) pairs are
 *           accumulated in a flat buffer and sorted with std::sort when the
 *           batch is full.  Models the sort phase of a merge-based index
 *           construction (SST build, B-tree bulk-load, etc.).
 *
 * Architecture (same as split_transform_bench / convert_transform_bench):
 *   - 1 GiB shared input buffer of pipe-delimited CSV records with randomized
 *     field lengths to defeat branch prediction and L3 cache reuse.
 *   - Each worker thread reads from a staggered offset and processes records
 *     until the timer expires.
 *   - Composite key: raw bytes of selected fields concatenated with a 0x00
 *     byte separator; zero-padded to KEY_MAX bytes for sort-entry comparisons.
 *
 * Usage:
 *   ./augment_transform_bench <duration_s> <n_workers> <num_fields>
 *       <field_length> <mode> <field_idx0> [field_idx1 ...]
 *   mode: hash | sort
 *   field_idx: 0-based field position(s) to include in the index key
 *
 * Examples:
 *   ./augment_transform_bench 10 4 16 128 hash 2
 *   ./augment_transform_bench 10 4 16 128 sort 0 3 7
 *
 * Compile:
 *   g++ -O3 -std=c++11 -pthread -o augment_transform_bench augment_transform_bench.cc
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>    // std::sort
#include <new>          // std::bad_alloc

#define NS_PER_S          1000000000ULL
#define INPUT_BUFFER_SIZE (1024ULL * 1024ULL * 1024ULL)  // 1 GiB — defeats L3

// Hash table parameters (per thread, private)
#define HASH_TABLE_BITS   21                              // 2^21 = 2,097,152 slots
#define HASH_TABLE_SIZE   (1ULL << HASH_TABLE_BITS)
#define HASH_TABLE_MASK   (HASH_TABLE_SIZE - 1)
#define HASH_LOAD_RESET   (HASH_TABLE_SIZE * 3 / 4)      // flush at 75% fill

// Sort batch parameters (per thread, private)
#define SORT_BATCH_SIZE   65536                           // entries per sort batch
#define KEY_MAX           256                             // max composite key bytes

// ---------------------------------------------------------------------------
// Index mode
// ---------------------------------------------------------------------------
typedef enum {
    MODE_HASH = 0,
    MODE_SORT = 1,
} IndexMode;

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static char*    shared_input_buffer = NULL;
static uint64_t actual_input_size   = 0;

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash
// ---------------------------------------------------------------------------
static inline uint64_t fnv1a_64(const char* data, uint32_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;  // 0 is the empty-slot sentinel; map to 1
}

// ---------------------------------------------------------------------------
// Sort entry (fixed-size for uniform std::sort)
// ---------------------------------------------------------------------------
typedef struct {
    char     key[KEY_MAX];  // zero-padded composite key
    uint64_t record_id;     // logical record offset in input buffer
} SortEntry;

static bool sort_entry_cmp(const SortEntry& x, const SortEntry& y) {
    return memcmp(x.key, y.key, KEY_MAX) < 0;
}

// ---------------------------------------------------------------------------
// Worker args
//   index_fields and n_index_fields are set by main and read-only in workers.
// ---------------------------------------------------------------------------
typedef struct {
    int       tid;
    uint64_t  end_ns;
    uint32_t  num_fields;
    uint32_t  field_length;
    IndexMode mode;
    const uint32_t* index_fields;    // 0-based field positions to index
    uint32_t        n_index_fields;
    // output
    uint64_t  count;       // records inserted into index
    uint64_t  bytes_in;    // bytes of input consumed
    uint64_t  aux;         // hash: cumulative probe steps; sort: batches completed
    uint8_t   sink;
} WorkerArgs;

// ===========================================================================
// Input buffer builder
// ===========================================================================

/**
 * Fill 1 GiB with pipe-delimited CSV records.  Field lengths are randomized
 * within ±50% of field_length to defeat branch prediction across records.
 * Each field is filled with a repeating alphabetic character; the last field
 * of each record ends with '\n', all others with '|'.
 *
 * The first 20 bytes of each field carry a decimal representation of a
 * unique monotonically increasing counter so that composite keys differ
 * across records (making hash collisions rare and sort keys distinct).
 */
static void build_input_buffer(uint32_t num_fields, uint32_t field_length) {
    shared_input_buffer = (char*)malloc(INPUT_BUFFER_SIZE);
    if (!shared_input_buffer) {
        fprintf(stderr, "augment_transform_bench: failed to allocate 1 GiB input buffer\n");
        exit(1);
    }

    uint32_t record_size = num_fields * field_length;
    actual_input_size    = (INPUT_BUFFER_SIZE / record_size) * record_size;

    srand(42);
    uint64_t row_counter = 0;

    for (uint64_t offset = 0; offset < actual_input_size; offset += record_size) {
        char*    rec             = shared_input_buffer + offset;
        uint32_t current_rec_pos = 0;

        for (uint32_t f = 0; f < num_fields; f++) {
            uint32_t this_field_len;
            if (f == num_fields - 1) {
                this_field_len = record_size - current_rec_pos;
            } else {
                int variance     = (int)(field_length / 2);
                int random_delta = (rand() % (variance * 2 + 1)) - variance;
                this_field_len   = (uint32_t)((int)field_length + random_delta);
                if (this_field_len < 24) this_field_len = 24;

                uint32_t remaining_fields = num_fields - 1 - f;
                uint32_t max_allowed =
                    (record_size - current_rec_pos) - (remaining_fields * 24);
                if (this_field_len > max_allowed) this_field_len = max_allowed;
            }

            char* fp = rec + current_rec_pos;

            // Write a unique decimal prefix so composite keys differ per record.
            char num_buf[24];
            int  num_len = snprintf(num_buf, sizeof(num_buf), "%lu",
                                    (unsigned long)(row_counter * num_fields + f + 1));
            uint32_t payload = this_field_len - 1;  // last byte = delimiter
            if ((uint32_t)num_len > payload) num_len = (int)payload;
            memcpy(fp, num_buf, num_len);
            memset(fp + num_len, 'A' + (f % 26), payload - num_len);

            fp[this_field_len - 1] = (f == num_fields - 1) ? '\n' : '|';
            current_rec_pos += this_field_len;
        }
        row_counter++;
    }
}

// ===========================================================================
// Composite key helper
// ===========================================================================

/**
 * Concatenate the selected fields into key_buf (zero-padded to KEY_MAX).
 * Fields are separated by a 0x00 byte to prevent key collisions across
 * different field boundaries.  Trailing delimiters and spaces are stripped.
 *
 * Returns the number of significant (non-padded) bytes written.
 */
static inline uint32_t compose_key(
        char*        key_buf,
        const char** field_ptrs,
        const uint32_t* field_lens,
        const uint32_t* index_fields,
        uint32_t     n_index_fields) {

    uint32_t pos = 0;
    for (uint32_t i = 0; i < n_index_fields && pos < KEY_MAX; i++) {
        uint32_t f    = index_fields[i];
        uint32_t vlen = field_lens[f];
        // Strip trailing delimiter / padding
        while (vlen > 0 && (field_ptrs[f][vlen - 1] == '|' ||
                             field_ptrs[f][vlen - 1] == '\n' ||
                             field_ptrs[f][vlen - 1] == ' ')) {
            vlen--;
        }
        uint32_t copy_len = (vlen < KEY_MAX - pos) ? vlen : KEY_MAX - pos;
        memcpy(key_buf + pos, field_ptrs[f], copy_len);
        pos += copy_len;

        // Null-byte separator between key components
        if (i < n_index_fields - 1 && pos < KEY_MAX) {
            key_buf[pos++] = '\x00';
        }
    }
    // Zero-pad remainder for stable memcmp in sort mode
    if (pos < KEY_MAX) memset(key_buf + pos, 0, KEY_MAX - pos);
    return pos;
}

// ===========================================================================
// Worker: hash index build
// ===========================================================================
//
// Each thread maintains a private open-addressing hash table of HASH_TABLE_SIZE
// uint64_t slots (FNV-1a hashes of composite keys; 0 = empty).  On 75% fill,
// the table is zeroed to simulate a flush/compaction cycle.
//
// Collision probe steps are accumulated to report avg probe length, which is a
// useful signal for index selectivity and key distribution quality.
//
static void* worker_hash(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;

    // Private hash table
    uint64_t* table = (uint64_t*)calloc(HASH_TABLE_SIZE, sizeof(uint64_t));
    if (!table) {
        fprintf(stderr, "worker_hash[%d]: calloc failed for hash table\n", a->tid);
        return NULL;
    }
    uint64_t table_fill = 0;

    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t*    field_lens = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));
    char         key_buf[KEY_MAX];

    uint64_t count      = 0;
    uint64_t bytes_in   = 0;
    uint64_t probe_steps = 0;
    uint8_t  sink       = 0;

    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        // -- Parse CSV record into field_ptrs / field_lens --
        uint32_t parsed      = 0;
        uint32_t src_pos     = 0;
        uint32_t field_start = 0;

        while (src_pos < record_size && parsed < a->num_fields) {
            char c = src[src_pos];
            if (c == '|' || c == '\n') {
                field_ptrs[parsed] = src + field_start;
                field_lens[parsed] = src_pos - field_start;
                parsed++;
                field_start = src_pos + 1;
            }
            src_pos++;
        }
        if (parsed < a->num_fields && field_start < record_size) {
            field_ptrs[parsed] = src + field_start;
            field_lens[parsed] = record_size - field_start;
            parsed++;
        }

        // -- Validate all requested index fields are present --
        bool valid = true;
        for (uint32_t i = 0; i < a->n_index_fields; i++) {
            if (a->index_fields[i] >= parsed) { valid = false; break; }
        }
        if (!valid) goto next_hash;

        {
            // -- Compose composite key and hash it --
            compose_key(key_buf, field_ptrs, field_lens, a->index_fields,
                        a->n_index_fields);
            uint64_t h   = fnv1a_64(key_buf, KEY_MAX);
            uint64_t slot = h & HASH_TABLE_MASK;

            // -- Linear probe insert --
            while (table[slot] != 0) {
                slot = (slot + 1) & HASH_TABLE_MASK;
                probe_steps++;
            }
            table[slot] = h;
            table_fill++;

            // -- Flush table when 75% full --
            if (table_fill >= HASH_LOAD_RESET) {
                memset(table, 0, HASH_TABLE_SIZE * sizeof(uint64_t));
                table_fill = 0;
            }

            sink ^= (uint8_t)h;
        }

next_hash:
        count++;
        bytes_in += record_size;

        read_offset += record_size;
        if (read_offset >= actual_input_size) read_offset = 0;
    }

    a->count    = count;
    a->bytes_in = bytes_in;
    a->aux      = probe_steps;
    a->sink     = sink;

    free(table);
    free(field_ptrs);
    free(field_lens);
    return NULL;
}

// ===========================================================================
// Worker: sort-merge index build
// ===========================================================================
//
// Each thread accumulates (composite-key, record_id) SortEntry structs in a
// flat batch buffer.  When SORT_BATCH_SIZE entries have been collected, the
// batch is sorted with std::sort (lexicographic key comparison via memcmp).
// This models the sort phase of an SST/B-tree bulk-load index build.
//
// `aux` returns the number of completed sort batches; multiply by
// SORT_BATCH_SIZE to get the total number of sorted index entries.
//
static void* worker_sort(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;

    // Private sort batch buffer
    SortEntry* batch = (SortEntry*)malloc(SORT_BATCH_SIZE * sizeof(SortEntry));
    if (!batch) {
        fprintf(stderr, "worker_sort[%d]: malloc failed for sort batch\n", a->tid);
        return NULL;
    }
    uint32_t batch_fill = 0;

    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t*    field_lens = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    uint64_t count         = 0;
    uint64_t bytes_in      = 0;
    uint64_t batches_sorted = 0;
    uint8_t  sink          = 0;

    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        // -- Parse CSV record --
        uint32_t parsed      = 0;
        uint32_t src_pos     = 0;
        uint32_t field_start = 0;

        while (src_pos < record_size && parsed < a->num_fields) {
            char c = src[src_pos];
            if (c == '|' || c == '\n') {
                field_ptrs[parsed] = src + field_start;
                field_lens[parsed] = src_pos - field_start;
                parsed++;
                field_start = src_pos + 1;
            }
            src_pos++;
        }
        if (parsed < a->num_fields && field_start < record_size) {
            field_ptrs[parsed] = src + field_start;
            field_lens[parsed] = record_size - field_start;
            parsed++;
        }

        // -- Validate index fields --
        bool valid = true;
        for (uint32_t i = 0; i < a->n_index_fields; i++) {
            if (a->index_fields[i] >= parsed) { valid = false; break; }
        }
        if (!valid) goto next_sort;

        {
            // -- Compose composite key into next batch slot --
            SortEntry* e = &batch[batch_fill];
            compose_key(e->key, field_ptrs, field_lens,
                        a->index_fields, a->n_index_fields);
            e->record_id = count;
            batch_fill++;

            // -- Sort and flush when batch is full --
            if (batch_fill == SORT_BATCH_SIZE) {
                std::sort(batch, batch + SORT_BATCH_SIZE, sort_entry_cmp);
                sink       ^= (uint8_t)batch[0].key[0]; // Anti-elide
                batch_fill  = 0;
                batches_sorted++;
            }
        }

next_sort:
        count++;
        bytes_in += record_size;

        read_offset += record_size;
        if (read_offset >= actual_input_size) read_offset = 0;
    }

    // Flush any partial batch (don't sort — partial batches don't count)
    (void)batch_fill;

    a->count    = count;
    a->bytes_in = bytes_in;
    a->aux      = batches_sorted;
    a->sink     = sink;

    free(batch);
    free(field_ptrs);
    free(field_lens);
    return NULL;
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, const char** argv) {
    if (argc < 7) {
        fprintf(stderr,
            "Usage: ./augment_transform_bench <duration_s> <n_workers>"
            " <num_fields> <field_length> <mode> <field_idx0> [field_idx1 ...]\n"
            "  mode: hash | sort\n"
            "  field_idx: 0-based field position(s) to index on\n"
            "Examples:\n"
            "  ./augment_transform_bench 10 4 16 128 hash 2\n"
            "  ./augment_transform_bench 10 4 16 128 sort 0 3 7\n");
        return 1;
    }

    int      duration_s   = atoi(argv[1]);
    uint32_t n_workers    = (uint32_t)atoi(argv[2]);
    uint32_t num_fields   = (uint32_t)atoi(argv[3]);
    uint32_t field_length = (uint32_t)atoi(argv[4]);
    const char* mode_str  = argv[5];

    IndexMode mode;
    if (strcmp(mode_str, "hash") == 0) {
        mode = MODE_HASH;
    } else if (strcmp(mode_str, "sort") == 0) {
        mode = MODE_SORT;
    } else {
        fprintf(stderr, "Unknown mode '%s'. Use: hash | sort\n", mode_str);
        return 1;
    }

    // Parse field indices (argv[6] onward)
    uint32_t n_index_fields = (uint32_t)(argc - 6);
    uint32_t* index_fields  = (uint32_t*)malloc(n_index_fields * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_index_fields; i++) {
        index_fields[i] = (uint32_t)atoi(argv[6 + i]);
        if (index_fields[i] >= num_fields) {
            fprintf(stderr,
                "ERROR: field_idx %u is out of range (num_fields=%u)\n",
                index_fields[i], num_fields);
            return 1;
        }
    }

    if (duration_s <= 0 || n_workers == 0 || num_fields == 0 || field_length == 0) {
        fprintf(stderr, "All numeric parameters must be > 0\n");
        return 1;
    }

    // Print index key description
    char idx_desc[256] = {0};
    uint32_t dpos = 0;
    for (uint32_t i = 0; i < n_index_fields && dpos < sizeof(idx_desc) - 8; i++) {
        dpos += snprintf(idx_desc + dpos, sizeof(idx_desc) - dpos,
                         "%s%u", i > 0 ? "," : "", index_fields[i]);
    }

    printf("augment_transform_bench: building 1 GiB input buffer...\n");
    build_input_buffer(num_fields, field_length);

    printf("augment_transform_bench: %u worker(s) × %ds\n"
           "  Record:    %u fields × %u B = %u B total\n"
           "  Mode:      %s\n"
           "  Index key: field[%s] (%u-component composite)\n",
           n_workers, duration_s,
           num_fields, field_length, num_fields * field_length,
           mode_str, idx_desc, n_index_fields);

    if (mode == MODE_HASH) {
        printf("  Hash table: %llu slots (%.0f MiB/thread), flush at 75%% fill\n",
               (unsigned long long)HASH_TABLE_SIZE,
               (double)(HASH_TABLE_SIZE * sizeof(uint64_t)) / (1024.0 * 1024.0));
    } else {
        printf("  Sort batch: %d entries (%.0f MiB/thread), KEY_MAX=%d B\n",
               SORT_BATCH_SIZE,
               (double)(SORT_BATCH_SIZE * sizeof(SortEntry)) / (1024.0 * 1024.0),
               KEY_MAX);
    }

    uint64_t end_ns = get_ns() + (uint64_t)duration_s * NS_PER_S;

    WorkerArgs* args    = (WorkerArgs*)calloc(n_workers, sizeof(WorkerArgs));
    pthread_t*  threads = (pthread_t*)calloc(n_workers, sizeof(pthread_t));

    void* (*worker_fn)(void*) = (mode == MODE_HASH) ? worker_hash : worker_sort;

    for (uint32_t i = 0; i < n_workers; i++) {
        args[i].tid            = (int)i;
        args[i].end_ns         = end_ns;
        args[i].num_fields     = num_fields;
        args[i].field_length   = field_length;
        args[i].mode           = mode;
        args[i].index_fields   = index_fields;
        args[i].n_index_fields = n_index_fields;
        pthread_create(&threads[i], NULL, worker_fn, &args[i]);
    }

    for (uint32_t i = 0; i < n_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t total_count    = 0;
    uint64_t total_bytes_in = 0;
    uint64_t total_aux      = 0;
    uint8_t  sink_accum     = 0;
    for (uint32_t i = 0; i < n_workers; i++) {
        total_count    += args[i].count;
        total_bytes_in += args[i].bytes_in;
        total_aux      += args[i].aux;
        sink_accum     ^= args[i].sink;
    }
    (void)sink_accum;

    free(args);
    free(threads);
    free(index_fields);
    free(shared_input_buffer);

    double rate_rps = (double)total_count    / (double)duration_s;
    double rate_mbs = (double)total_bytes_in / (double)duration_s / (1024.0 * 1024.0);

    printf("augment_transform_bench: workers=%u  records=%lu"
           "  rate=%.0f rec/s  input_bw=%.1f MB/s\n",
           n_workers,
           (unsigned long)total_count,
           rate_rps,
           rate_mbs);

    if (mode == MODE_HASH) {
        double avg_probe = (total_count > 0)
            ? (double)total_aux / (double)total_count : 0.0;
        printf("  Hash collisions: total_probe_steps=%lu  avg_probe=%.4f steps/insert\n",
               (unsigned long)total_aux, avg_probe);
    } else {
        uint64_t total_sorted = total_aux * (uint64_t)SORT_BATCH_SIZE;
        printf("  Sort batches: completed=%lu  entries_sorted=%lu\n",
               (unsigned long)total_aux, (unsigned long)total_sorted);
    }

    return 0;
}
