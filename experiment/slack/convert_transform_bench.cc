/**
 * convert_transform_bench.cc — Out-of-cache format conversion benchmark
 *
 * Simulates a conversion transformation on data that does NOT fit in CPU cache,
 * forcing real memory bandwidth utilization during format parsing and emission.
 *
 * Supported conversion modes:
 *   csv2json  — Parse pipe-delimited CSV records, emit as JSON objects with
 *               synthesized field names ("field_1", "field_2", …).
 *   json2csv  — Parse minimal JSON objects (key:"value" pairs), emit the values
 *               as pipe-delimited CSV rows (keys dropped).
 *   coerce    — Parse pipe-delimited CSV records, treat every field as a numeric
 *               string, convert to int64 or double via strtoll/strtod, then
 *               re-serialize with snprintf. Models the dominant cost of numeric
 *               type coercion in HTAP scan/projection pipelines.
 *
 * Architecture (same as split_transform_bench.cc):
 *   - 1 GiB shared input buffer pre-generated with randomized field lengths to
 *     defeat branch prediction and instruction cache reuse across records.
 *   - Each worker thread reads sequentially from a staggered offset, converting
 *     records into a per-thread 16 MiB ring buffer (wrapped on overflow).
 *   - Conversion throughput reported as records/s and MB/s of input consumed.
 *
 * Usage:
 *   ./convert_transform_bench <duration_s> <n_workers> <num_fields> <field_length> <mode>
 *   mode: csv2json | json2csv | coerce
 *
 * Compile:
 *   g++ -O3 -pthread -o convert_transform_bench convert_transform_bench.cc
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>   // for strtod, snprintf of doubles

#define NS_PER_S          1000000000ULL
#define INPUT_BUFFER_SIZE (1024ULL * 1024ULL * 1024ULL) // 1 GiB — defeats L3
#define RING_BUFFER_SIZE  (16ULL  * 1024ULL * 1024ULL)  // 16 MiB per thread

// ---------------------------------------------------------------------------
// Conversion mode enum
// ---------------------------------------------------------------------------
typedef enum {
    MODE_CSV2JSON = 0,
    MODE_JSON2CSV = 1,
    MODE_COERCE   = 2,
} ConvertMode;

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
// Worker args
// ---------------------------------------------------------------------------
typedef struct {
    int         tid;
    uint64_t    end_ns;
    uint32_t    num_fields;
    uint32_t    field_length;
    ConvertMode mode;
    uint64_t    count;        // records converted
    uint64_t    bytes_in;     // bytes of input consumed
    uint8_t     sink;
} WorkerArgs;

// ===========================================================================
// Input buffer builders
// ===========================================================================

/**
 * Build 1 GiB of pipe-delimited CSV records with randomized field lengths.
 * Each field is padded with a repeating character; delimiter is '|'; row ends
 * with '\n'.  Used for csv2json and coerce modes.
 *
 * Numeric fields for coerce mode: the first 16 bytes of each field are written
 * as a decimal integer string (right-padded with spaces to field_length) so
 * that strtoll can parse a realistic numeric token.
 */
static void build_input_buffer_csv(uint32_t num_fields, uint32_t field_length,
                                   bool numeric_values) {
    shared_input_buffer = (char*)malloc(INPUT_BUFFER_SIZE);
    if (!shared_input_buffer) {
        fprintf(stderr, "Failed to allocate 1 GiB input buffer.\n");
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
                if (this_field_len < 16) this_field_len = 16;

                uint32_t remaining_fields = num_fields - 1 - f;
                uint32_t max_allowed =
                    (record_size - current_rec_pos) - (remaining_fields * 16);
                if (this_field_len > max_allowed) this_field_len = max_allowed;
            }

            char* fp = rec + current_rec_pos;

            if (numeric_values) {
                // Write a decimal integer that strtoll can parse; pad remainder.
                char num_buf[32];
                int  num_len = snprintf(num_buf, sizeof(num_buf), "%lu",
                                        (unsigned long)(row_counter * num_fields + f + 1));
                // Copy number, then space-pad, then delimiter
                uint32_t payload = this_field_len - 1; // last byte is delimiter
                if ((uint32_t)num_len > payload) num_len = (int)payload;
                memcpy(fp, num_buf, num_len);
                memset(fp + num_len, ' ', payload - num_len);
            } else {
                // Alphabetic filler
                memset(fp, 'A' + (f % 26), this_field_len - 1);
            }

            fp[this_field_len - 1] = (f == num_fields - 1) ? '\n' : '|';
            current_rec_pos += this_field_len;
        }
        row_counter++;
    }
}

/**
 * Build 1 GiB of minimal JSON records: {"field_1":"AAA…","field_2":"BBB…"}\n
 * Randomized value lengths, same distribution as CSV builder.
 * Used for json2csv mode.
 */
static void build_input_buffer_json(uint32_t num_fields, uint32_t field_length) {
    shared_input_buffer = (char*)malloc(INPUT_BUFFER_SIZE);
    if (!shared_input_buffer) {
        fprintf(stderr, "Failed to allocate 1 GiB input buffer.\n");
        exit(1);
    }

    uint32_t record_size = num_fields * field_length;
    actual_input_size    = (INPUT_BUFFER_SIZE / record_size) * record_size;

    srand(42);
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
                if (this_field_len < 32) this_field_len = 32;

                uint32_t remaining_fields = num_fields - 1 - f;
                uint32_t max_allowed =
                    (record_size - current_rec_pos) - (remaining_fields * 32);
                if (this_field_len > max_allowed) this_field_len = max_allowed;
            }

            char* fp = rec + current_rec_pos;

            // Key prefix:  {"field_1":"  or  "field_N":"
            char prefix[64];
            int  prefix_len;
            if (f == 0) {
                prefix_len = snprintf(prefix, sizeof(prefix),
                                      "{\"field_%u\":\"", f + 1);
            } else {
                prefix_len = snprintf(prefix, sizeof(prefix),
                                      "\"field_%u\":\"", f + 1);
            }

            // Suffix: "  or  "}  or  "}\n
            // Close-quote + delimiter (2 bytes for interior, 3 for last)
            int suffix_len = (f == num_fields - 1) ? 3 : 2;
            int padding    = (int)this_field_len - prefix_len - suffix_len;
            if (padding < 1) padding = 1;

            memcpy(fp, prefix, prefix_len);
            memset(fp + prefix_len, 'A' + (f % 26), padding);

            char* sp = fp + prefix_len + padding;
            if (f == num_fields - 1) {
                sp[0] = '"'; sp[1] = '}'; sp[2] = '\n';
                // fill any leftover bytes
                for (int i = 3; i < (int)this_field_len - prefix_len - padding; i++)
                    sp[i] = ' ';
            } else {
                sp[0] = '"'; sp[1] = ',';
                for (int i = 2; i < (int)this_field_len - prefix_len - padding; i++)
                    sp[i] = ' ';
            }

            current_rec_pos += this_field_len;
        }
    }
}

// ===========================================================================
// Worker: CSV → JSON
// ===========================================================================
static void* worker_csv2json(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;

    char*    ring_buf    = (char*)malloc(RING_BUFFER_SIZE);
    uint64_t ring_offset = 0;

    // Pre-build the per-field JSON key prefixes (e.g., "field_1":"  ) once.
    char** key_bufs = (char**)malloc(a->num_fields * sizeof(char*));
    int*   key_lens = (int*)malloc(a->num_fields * sizeof(int));
    for (uint32_t f = 0; f < a->num_fields; f++) {
        key_bufs[f] = (char*)malloc(64);
        if (f == 0) {
            key_lens[f] = snprintf(key_bufs[f], 64, "{\"field_%u\":\"", f + 1);
        } else {
            key_lens[f] = snprintf(key_bufs[f], 64, "\"field_%u\":\"", f + 1);
        }
    }

    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t*    field_lens = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    uint64_t count    = 0;
    uint64_t bytes_in = 0;
    uint8_t  sink     = 0;

    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        // Ensure ring buffer has room for a worst-case output record.
        // Worst case: num_fields * (key_overhead + field_length + 2) + 4
        uint32_t max_out = a->num_fields * ((int)a->field_length + 64) + 8;
        if (ring_offset + max_out >= RING_BUFFER_SIZE) ring_offset = 0;

        // 1. Parse CSV record into fields (pipe or newline delimited)
        uint32_t parsed = 0;
        uint32_t src_pos = 0;
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
        // Capture final field if no trailing delimiter
        if (parsed < a->num_fields && field_start < record_size) {
            field_ptrs[parsed] = src + field_start;
            field_lens[parsed] = record_size - field_start;
            parsed++;
        }

        // 2. Emit JSON record
        char*    dest       = ring_buf + ring_offset;
        uint32_t dest_off   = 0;

        for (uint32_t f = 0; f < parsed; f++) {
            // Write key prefix
            memcpy(dest + dest_off, key_bufs[f], key_lens[f]);
            dest_off += key_lens[f];

            // Write value (strip trailing spaces from numeric padding)
            uint32_t vlen = field_lens[f];
            while (vlen > 0 && field_ptrs[f][vlen - 1] == ' ') vlen--;
            memcpy(dest + dest_off, field_ptrs[f], vlen);
            dest_off += vlen;

            // Close: "  or  "}\n
            if (f == parsed - 1) {
                dest[dest_off++] = '"';
                dest[dest_off++] = '}';
                dest[dest_off++] = '\n';
            } else {
                dest[dest_off++] = '"';
                dest[dest_off++] = ',';
            }
        }

        ring_offset += dest_off;
        sink        ^= dest[0]; // Anti-elide
        count++;
        bytes_in += record_size;

        read_offset += record_size;
        if (read_offset >= actual_input_size) read_offset = 0;
    }

    a->count    = count;
    a->bytes_in = bytes_in;
    a->sink     = sink;

    for (uint32_t f = 0; f < a->num_fields; f++) free(key_bufs[f]);
    free(key_bufs);
    free(key_lens);
    free(ring_buf);
    free(field_ptrs);
    free(field_lens);
    return NULL;
}

// ===========================================================================
// Worker: JSON → CSV
// ===========================================================================
static void* worker_json2csv(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;

    char*    ring_buf    = (char*)malloc(RING_BUFFER_SIZE);
    uint64_t ring_offset = 0;

    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t*    field_lens = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    uint64_t count    = 0;
    uint64_t bytes_in = 0;
    uint8_t  sink     = 0;

    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        uint32_t max_out = record_size + 16;
        if (ring_offset + max_out >= RING_BUFFER_SIZE) ring_offset = 0;

        // 1. Parse JSON: extract value strings only (skip keys).
        //    Strategy: scan for ':' then collect the quoted value after it.
        uint32_t parsed  = 0;
        uint32_t src_pos = 0;

        // Skip leading '{'
        if (src_pos < record_size && src[src_pos] == '{') src_pos++;

        while (src_pos < record_size && parsed < a->num_fields) {
            // Advance to the ':' separating key from value
            while (src_pos < record_size && src[src_pos] != ':') src_pos++;
            if (src_pos >= record_size) break;
            src_pos++; // skip ':'

            // Skip whitespace
            while (src_pos < record_size && src[src_pos] == ' ') src_pos++;

            // Expect opening '"' for value
            if (src_pos < record_size && src[src_pos] == '"') {
                src_pos++; // skip opening quote
                uint32_t val_start = src_pos;
                // Scan to closing '"'
                while (src_pos < record_size && src[src_pos] != '"') src_pos++;
                field_ptrs[parsed] = src + val_start;
                field_lens[parsed] = src_pos - val_start;
                parsed++;
                if (src_pos < record_size) src_pos++; // skip closing quote
            }

            // Skip ',' or '}' separator
            while (src_pos < record_size &&
                   (src[src_pos] == ',' || src[src_pos] == ' ')) src_pos++;
        }

        // 2. Emit CSV row (pipe-delimited, '\n' terminated)
        char*    dest     = ring_buf + ring_offset;
        uint32_t dest_off = 0;

        for (uint32_t f = 0; f < parsed; f++) {
            memcpy(dest + dest_off, field_ptrs[f], field_lens[f]);
            dest_off += field_lens[f];
            dest[dest_off++] = (f == parsed - 1) ? '\n' : '|';
        }

        ring_offset += dest_off;
        sink        ^= dest[0];
        count++;
        bytes_in += record_size;

        read_offset += record_size;
        if (read_offset >= actual_input_size) read_offset = 0;
    }

    a->count    = count;
    a->bytes_in = bytes_in;
    a->sink     = sink;

    free(ring_buf);
    free(field_ptrs);
    free(field_lens);
    return NULL;
}

// ===========================================================================
// Worker: Numeric type coercion (CSV string → int64 → re-serialized string)
// ===========================================================================
//
// Models the cost of: parse a decimal string field → strtoll → snprintf back.
// This is the dominant kernel in HTAP scan projections that re-type columns
// from the wire format (string) into an analytic column store (integer/float).
//
static void* worker_coerce(void* arg) {
    WorkerArgs* a = (WorkerArgs*)arg;

    uint32_t record_size = a->num_fields * a->field_length;

    char*    ring_buf    = (char*)malloc(RING_BUFFER_SIZE);
    uint64_t ring_offset = 0;

    const char** field_ptrs = (const char**)malloc(a->num_fields * sizeof(const char*));
    uint32_t*    field_lens = (uint32_t*)malloc(a->num_fields * sizeof(uint32_t));

    // Temporary buffer for coerced string re-serialization
    char coerce_buf[32];

    uint64_t count    = 0;
    uint64_t bytes_in = 0;
    uint8_t  sink     = 0;

    uint64_t read_offset = (a->tid * (actual_input_size / 8)) % actual_input_size;

    while (get_ns() < a->end_ns) {
        const char* src = shared_input_buffer + read_offset;

        uint32_t max_out = a->num_fields * 24 + 8;
        if (ring_offset + max_out >= RING_BUFFER_SIZE) ring_offset = 0;

        // 1. Parse CSV record
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

        // 2. Coerce each field: string → int64 → string
        char*    dest     = ring_buf + ring_offset;
        uint32_t dest_off = 0;

        for (uint32_t f = 0; f < parsed; f++) {
            // strtoll parses up to the first non-digit; space-padded fields
            // return a valid integer (0 for all-space fields is fine for
            // cost modeling — the branch misprediction and memory touch are
            // what we're measuring).
            char* endptr;
            long long val = strtoll(field_ptrs[f], &endptr, 10);

            int out_len = snprintf(coerce_buf, sizeof(coerce_buf), "%lld", val);
            if (out_len < 0) out_len = 0;
            if ((uint32_t)out_len > sizeof(coerce_buf) - 1)
                out_len = (int)sizeof(coerce_buf) - 1;

            memcpy(dest + dest_off, coerce_buf, out_len);
            dest_off += out_len;
            dest[dest_off++] = (f == parsed - 1) ? '\n' : '|';
        }

        ring_offset += dest_off;
        sink        ^= dest[0];
        count++;
        bytes_in += record_size;

        read_offset += record_size;
        if (read_offset >= actual_input_size) read_offset = 0;
    }

    a->count    = count;
    a->bytes_in = bytes_in;
    a->sink     = sink;

    free(ring_buf);
    free(field_ptrs);
    free(field_lens);
    return NULL;
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, const char** argv) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: ./convert_transform_bench <duration_s> <n_workers>"
            " <num_fields> <field_length> <mode>\n"
            "  mode: csv2json | json2csv | coerce\n"
            "Example: ./convert_transform_bench 10 4 16 128 csv2json\n");
        return 1;
    }

    int      duration_s  = atoi(argv[1]);
    uint32_t n_workers   = (uint32_t)atoi(argv[2]);
    uint32_t num_fields  = (uint32_t)atoi(argv[3]);
    uint32_t field_length = (uint32_t)atoi(argv[4]);
    const char* mode_str = argv[5];

    ConvertMode mode;
    if (strcmp(mode_str, "csv2json") == 0) {
        mode = MODE_CSV2JSON;
    } else if (strcmp(mode_str, "json2csv") == 0) {
        mode = MODE_JSON2CSV;
    } else if (strcmp(mode_str, "coerce") == 0) {
        mode = MODE_COERCE;
    } else {
        fprintf(stderr, "Unknown mode '%s'. Use: csv2json | json2csv | coerce\n",
                mode_str);
        return 1;
    }

    if (duration_s <= 0 || n_workers == 0 || num_fields == 0 || field_length == 0) {
        fprintf(stderr, "All numeric parameters must be > 0\n");
        return 1;
    }

    // Build input buffer
    printf("convert_transform_bench: building 1 GiB input buffer (mode=%s)...\n",
           mode_str);
    if (mode == MODE_JSON2CSV) {
        build_input_buffer_json(num_fields, field_length);
    } else {
        // csv2json and coerce both read from CSV input;
        // coerce mode uses numeric values so strtoll has something to parse.
        build_input_buffer_csv(num_fields, field_length, /*numeric=*/mode == MODE_COERCE);
    }

    printf("convert_transform_bench: %u worker(s) × %ds\n"
           "  Record: %u fields × %u B = %u B total\n"
           "  Mode:   %s\n",
           n_workers, duration_s,
           num_fields, field_length, num_fields * field_length,
           mode_str);

    uint64_t end_ns = get_ns() + (uint64_t)duration_s * NS_PER_S;

    WorkerArgs* args    = (WorkerArgs*)calloc(n_workers, sizeof(WorkerArgs));
    pthread_t*  threads = (pthread_t*)calloc(n_workers, sizeof(pthread_t));

    void* (*worker_fn)(void*);
    switch (mode) {
        case MODE_CSV2JSON: worker_fn = worker_csv2json; break;
        case MODE_JSON2CSV: worker_fn = worker_json2csv; break;
        case MODE_COERCE:   worker_fn = worker_coerce;   break;
        default:            worker_fn = worker_csv2json; break;
    }

    for (uint32_t i = 0; i < n_workers; i++) {
        args[i].tid          = (int)i;
        args[i].end_ns       = end_ns;
        args[i].num_fields   = num_fields;
        args[i].field_length = field_length;
        args[i].mode         = mode;
        args[i].count        = 0;
        args[i].bytes_in     = 0;
        args[i].sink         = 0;
        pthread_create(&threads[i], NULL, worker_fn, &args[i]);
    }

    for (uint32_t i = 0; i < n_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t total_records  = 0;
    uint64_t total_bytes_in = 0;
    uint8_t  sink_accum     = 0;
    for (uint32_t i = 0; i < n_workers; i++) {
        total_records  += args[i].count;
        total_bytes_in += args[i].bytes_in;
        sink_accum     ^= args[i].sink;
    }
    (void)sink_accum;

    free(args);
    free(threads);
    free(shared_input_buffer);

    double rate_rps = (double)total_records  / (double)duration_s;
    double rate_mbs = (double)total_bytes_in / (double)duration_s / (1024.0 * 1024.0);

    printf("convert_transform_bench: workers=%u  records=%lu"
           "  rate=%.0f rec/s  input_bw=%.1f MB/s\n",
           n_workers,
           (unsigned long)total_records,
           rate_rps,
           rate_mbs);

    return 0;
}
