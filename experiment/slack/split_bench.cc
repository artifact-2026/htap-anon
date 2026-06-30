/**
 * split_bench.cc — In-memory CSV column-split throughput benchmark
 *
 * Models a Mycelium "split" transformation in pure CPU, no disk I/O.
 *
 * Record layout (2048 bytes total, 16 fields × 128 bytes each):
 *
 *   f00=<123 bytes of fill>|f01=<123 bytes of fill>|...|f15=<123 bytes>\0
 *   |_____________________|
 *        128 bytes
 *
 *   - Bytes 0..3   : field header "fNN="
 *   - Bytes 4..126 : 123-byte value payload (repeated fill character)
 *   - Byte  127    : '|' separator (or '\0' for the last field)
 *
 * Split operation (per record):
 *   1. Scan for the 8th '|' to locate the field boundary (always at byte 1023,
 *      but the scan reads through the first 1 KiB — representative of real
 *      delimiter parsing work).
 *   2. Copy fields 0-7  (bytes [0,   1023]) into half_a (1 KiB output).
 *   3. Copy fields 8-15 (bytes [1024, 2047]) into half_b (1 KiB output).
 *
 * Each worker thread operates on its own private record and output buffers —
 * no sharing, no false sharing, no locks.  The anti-elide sink is per-thread
 * and folded together after all threads join.
 *
 * Usage:
 *   ./split_bench <duration_s> [n_workers]
 *
 *   duration_s   run time in seconds
 *   n_workers    parallel split worker threads (default: 1)
 *
 * Output (stdout, one summary line):
 *   split_bench: workers=N  splits=X  rate=Y splits/s
 *
 * Compile:
 *   g++ -O2 -pthread -o split_bench split_bench.cc -lrt
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_S    (1000000000ULL)

#define FIELDS      16
#define FIELD_SIZE  128                        /* bytes per field including separator */
#define RECORD_SIZE (FIELDS * FIELD_SIZE)      /* 2048 bytes */
#define SPLIT_AT    (FIELDS / 2)               /* split after field 7 → 8+8 */
#define HALF_SIZE   (SPLIT_AT * FIELD_SIZE)    /* 1024 bytes per output half */

static inline uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

/**
 * Build a 2 KiB CSV record into buf (must be RECORD_SIZE bytes).
 *
 * Each field occupies exactly FIELD_SIZE bytes:
 *   - 4-byte header  "fNN="
 *   - 123-byte value (fill character, different per field so fields are distinct)
 *   - 1-byte separator '|' (or '\0' for the last field)
 */
static void build_record(char *buf) {
    for (int f = 0; f < FIELDS; f++) {
        char *fp = buf + f * FIELD_SIZE;
        fp[0] = 'f';
        fp[1] = '0' + f / 10;
        fp[2] = '0' + f % 10;
        fp[3] = '=';
        memset(fp + 4, 'A' + (f % 26), 123);
        fp[127] = (f < FIELDS - 1) ? '|' : '\0';
    }
}

/**
 * Split one record into two equal halves.
 *
 * Scans forward through the record to find the SPLIT_AT-th '|' delimiter,
 * which marks the end of the first half.  This linear scan (reading up to
 * 1 KiB of input) represents the parsing cost of a real column-split
 * transformer.  With fixed 128-byte fields the delimiter is always at byte
 * 1023, but the CPU still has to read through those bytes — equivalent to
 * iterating over field headers and values in a real parser.
 *
 * half_a receives fields 0..(SPLIT_AT-1), half_b receives fields
 * SPLIT_AT..(FIELDS-1).  Both output buffers must be at least HALF_SIZE+1
 * bytes (the +1 is for the NUL terminator written to half_a).
 *
 * Returns 1 on success, 0 if the SPLIT_AT-th delimiter was not found
 * (should never happen with a well-formed record).
 */
static inline int split_record(const char * __restrict__ rec,
                                char       * __restrict__ half_a,
                                char       * __restrict__ half_b) {
    /* Find the SPLIT_AT-th '|' to locate the boundary between the two halves. */
    int found = 0;
    int boundary = -1;
    for (int i = 0; i < RECORD_SIZE; i++) {
        if (rec[i] == '|' && ++found == SPLIT_AT) {
            boundary = i + 1;   /* first byte of the second half */
            break;
        }
    }
    if (__builtin_expect(boundary < 0, 0)) return 0;

    /* Copy each half into its output buffer and NUL-terminate. */
    memcpy(half_a, rec,             boundary - 1);
    half_a[boundary - 1] = '\0';
    memcpy(half_b, rec + boundary,  RECORD_SIZE - boundary);
    half_b[RECORD_SIZE - boundary]  = '\0';
    return 1;
}

/* ── Per-thread state ─────────────────────────────────────────────────────── */

typedef struct {
    int      tid;
    uint64_t end_ns;
    uint64_t count;   /* total splits completed */
    uint8_t  sink;    /* anti-elide: accumulated byte from outputs */
} WorkerArgs;

static void *worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

    /* Private buffers — no sharing between threads.
     * Align to cache line (64 bytes) to avoid false sharing on the stack.
     * RECORD_SIZE = 2048 B, HALF_SIZE = 1024 B — both fit comfortably in L1. */
    char __attribute__((aligned(64))) rec[RECORD_SIZE];
    char __attribute__((aligned(64))) half_a[HALF_SIZE + 1];
    char __attribute__((aligned(64))) half_b[HALF_SIZE + 1];

    build_record(rec);

    uint64_t count = 0;
    uint8_t  sink  = 0;

    while (get_ns() < a->end_ns) {
        split_record(rec, half_a, half_b);
        /* Anti-elide: XOR one byte from each output so the compiler cannot
         * prove the split results are unused and elide the work. */
        sink ^= (uint8_t)half_a[0] ^ (uint8_t)half_b[0];
        count++;
    }

    a->count = count;
    a->sink  = sink;
    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./split_bench <duration_s> [n_workers]\n"
            "\n"
            "  duration_s   run time in seconds\n"
            "  n_workers    parallel split worker threads (default: 1)\n"
            "\n"
            "Record: %d fields × %d B = %d B total\n"
            "Split:  fields 0-%d → half_a (%d B)\n"
            "        fields %d-%d → half_b (%d B)\n"
            "All data is generated and consumed in memory — no disk I/O.\n",
            FIELDS, FIELD_SIZE, RECORD_SIZE,
            SPLIT_AT - 1, HALF_SIZE,
            SPLIT_AT, FIELDS - 1, HALF_SIZE);
        return 1;
    }

    int      duration_s = atoi(argv[1]);
    uint32_t n_workers  = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 1;

    if (duration_s <= 0 || n_workers == 0) {
        fprintf(stderr, "ERROR: duration and n_workers must be > 0\n");
        return 1;
    }

    fprintf(stderr,
        "split_bench: %u worker(s) × %ds  "
        "record=%dB (%d fields×%dB)  split=%d+%d fields\n",
        n_workers, duration_s,
        RECORD_SIZE, FIELDS, FIELD_SIZE,
        SPLIT_AT, FIELDS - SPLIT_AT);

    uint64_t end_ns = get_ns() + (uint64_t)duration_s * NS_PER_S;

    WorkerArgs *args    = (WorkerArgs *)calloc(n_workers, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(n_workers, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    for (uint32_t i = 0; i < n_workers; i++) {
        args[i].tid    = (int)i;
        args[i].end_ns = end_ns;
        args[i].count  = 0;
        args[i].sink   = 0;
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
            return 1;
        }
    }

    for (uint32_t i = 0; i < n_workers; i++)
        pthread_join(threads[i], NULL);

    uint64_t total_splits = 0;
    uint8_t  sink_accum   = 0;
    for (uint32_t i = 0; i < n_workers; i++) {
        total_splits += args[i].count;
        sink_accum   ^= args[i].sink;
    }
    (void)sink_accum;   /* prevent elision */

    free(args);
    free(threads);

    double rate = (double)total_splits / (double)duration_s;
    printf("split_bench: workers=%u  splits=%lu  rate=%.0f splits/s\n",
           n_workers, (unsigned long)total_splits, rate);
    return 0;
}
