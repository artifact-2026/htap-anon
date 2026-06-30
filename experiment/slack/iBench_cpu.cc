/** $lic$
 * Copyright (C) 2016-2017 by The Board of Trustees of Cornell University
 * Copyright (C) 2013-2016 by The Board of Trustees of Stanford University
 *
 * This file is part of iBench.
 *
 * iBench is free software; you can redistribute it and/or modify it under the
 * terms of the Modified BSD-3 License as published by the Open Source Initiative.
 *
 * If you use this software in your research, we request that you reference
 * the iBench paper ("iBench: Quantifying Interference for Datacenter Applications",
 * Delimitrou and Kozyrakis, IISWC'13, September 2013) as the source of the benchmark
 * suite in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * iBench is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the BSD-3 License for more details.
 *
 * You should have received a copy of the Modified BSD-3 License along with
 * this program. If not, see <https://opensource.org/licenses/BSD-3-Clause>.
 **/

/**
 * CPU interference micro-benchmark — Mycelium JSON→CSV transform variant.
 *
 * Work unit (one "operation"):
 *
 *   transform_record(json_in, half_a_out, half_b_out)   // JSON→CSV split
 *   serialize_halves(half_a, half_b, proto_buf)          // CSV→Protobuf serialization
 *   json_to_proto(json_in, proto_direct)                 // JSON→Protobuf (direct)
 *
 *   1. JSON parse   — scan a synthetic 16-field JSON record character by
 *                     character to extract field values.  Alternating fields
 *                     are numeric (double) or alphanumeric string.
 *   2. String→num   — strtod() each numeric field (8 of the 16 fields).
 *   3. CSV format   — snprintf("%.2f") each converted double back to a
 *                     decimal string; memcpy() each string field verbatim.
 *   4. Column split — first 8 fields (CSV) go into half_a, last 8 into half_b,
 *                     mirroring Mycelium's column-split transformation.
 *   5. Serialization — encode half_a (field 1) and half_b (field 2) as
 *                     protobuf wire-format length-delimited fields (wire type 2).
 *   6. JSON→Protobuf — re-parse the original JSON and encode each field
 *                     directly into protobuf wire format: numeric fields as
 *                     wire type 1 (64-bit IEEE 754 double), string fields as
 *                     wire type 2 (length-delimited).  Represents the fast
 *                     path that skips the CSV intermediate entirely.
 *
 * Steps 5 and 6 use a hand-rolled protobuf wire encoder (varint + wire types
 * 1 and 2) — no external protobuf library required.
 *
 * This pipeline is heavier per operation than the original Murmur3 mix-round
 * loop because strtod/snprintf each involve significant arithmetic (multiply/
 * divide chains for base-10 ↔ binary conversion), and the JSON scan forces a
 * sequential read of the full ~1 KB input record.
 *
 * A background reporter thread wakes every second, sums ops_count across all
 * worker threads, and prints the per-second throughput.  At exit it emits a
 * summary line parseable by the cpu_slack_sweep.sh summariser:
 *
 *   cpu_throughput mean: X stddev: Y min: Z max: W
 *
 * Usage:
 *   ./cpu <duration_s>
 *   ./cpu <duration_s> <n_threads>
 *   ./cpu <duration_s> <n_threads> <rate>
 *   ./cpu <duration_s> <n_threads> <rate> <cpu_id>
 *
 * Parameters:
 *   duration_s   run duration in seconds
 *   n_threads    worker thread count (float); integer part = full 100%-duty
 *                threads, fractional part = one extra thread at that fraction
 *                of one core.  Default: all online CPUs.
 *                e.g. 2 -> two busy-loop threads; 2.5 -> two full + one at 50%;
 *                     0.25 -> one thread at 25% duty (no full threads).
 *   rate         target transforms/s per FULL thread (0 = saturate, default).
 *                Ignored for the fractional thread, which uses duty-cycle
 *                control instead.
 *   cpu_id       base CPU for per-thread affinity via sched_setaffinity.
 *                Thread i is pinned to cpu_id + i.  -1 = no pinning (default).
 *
 * Compile:
 *   g++ -O2 -pthread -o cpu iBench_cpu.cc -lm -lrt
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#ifdef __linux__
#  include <sched.h>
#endif

/* ── Timing ─────────────────────────────────────────────────────────────── */

#define NS_PER_S (1000000000ULL)

static inline uint64_t getNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

static inline void sleepNs(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long)(ns % NS_PER_S);
    while (nanosleep(&ts, &ts) != 0)
        ;
}

/* ── Signal handling ─────────────────────────────────────────────────────── */

volatile sig_atomic_t g_keep_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/* ── JSON record definition ──────────────────────────────────────────────── */
/*
 * Synthetic JSON record: 64 fields × 32 bytes of payload per field,
 * alternating numeric (even) and string (odd):
 *
 *   {"f00":1000000.000000,"f01":"AAAA...A","f02":...,"f63":"..."}
 *    ^^^^ even = double ^^^^              ^^^^ odd = 32-char string ^^^^
 *
 * FIELD_COUNT      total columns
 * STR_VAL_LEN      string field width in bytes (32)
 * JSON_MAXLEN      upper bound for the generated JSON string
 *                  64 fields × ~42 chars avg + braces ≈ 2700 B; 4096 is safe.
 *
 * NUM_GROUPS       number of column groups after the split stage
 * FIELDS_PER_GROUP columns per group (FIELD_COUNT / NUM_GROUPS = 16)
 *
 * PROTO_GROUP_MAX  upper bound for one group's protobuf wire output:
 *   8 numeric fields × (1B tag + 8B value)          =  72 B
 *   8 string  fields × (1B tag + 1B len + 32B data) = 272 B
 *   Total ≈ 344 B; 512 with margin.
 */
#define FIELD_COUNT      64
#define STR_VAL_LEN      32
#define JSON_MAXLEN      4096
#define NUM_GROUPS       4
#define FIELDS_PER_GROUP (FIELD_COUNT / NUM_GROUPS)   /* 16 */
#define PROTO_GROUP_MAX  512

/**
 * Populate `buf` with a synthetic JSON record (64 fields × 32 B payload).
 * Even fields (0, 2, …, 62): numeric doubles with distinct values per field.
 * Odd  fields (1, 3, …, 63): 32-char alphanumeric strings; fill character
 *   cycles through A–Z with the field index so adjacent fields differ.
 */
static void build_json(char *buf) {
    int pos = 0;
    buf[pos++] = '{';
    for (int f = 0; f < FIELD_COUNT; f++) {
        if (f > 0) buf[pos++] = ',';
        /* Field key */
        pos += snprintf(buf + pos, JSON_MAXLEN - pos, "\"f%02d\":", f);
        if (f % 2 == 0) {
            /* Numeric field: distinct value per field index */
            double val = 1000000.0 * (f / 2 + 1) + 123456.789 * f;
            pos += snprintf(buf + pos, JSON_MAXLEN - pos, "%.6f", val);
        } else {
            /* String field: 80-char fill, fill character varies */
            char ch = 'A' + ((f / 2) % 26);
            buf[pos++] = '"';
            memset(buf + pos, ch, STR_VAL_LEN);
            pos += STR_VAL_LEN;
            buf[pos++] = '"';
        }
    }
    buf[pos++] = '}';
    buf[pos]   = '\0';
}

/* ── Protobuf wire-format helpers ────────────────────────────────────────── */
/*
 * Minimal protobuf wire encoder — no external library required.
 *
 * Wire types used:
 *   1  — 64-bit little-endian (TYPE_DOUBLE / TYPE_FIXED64)
 *   2  — length-delimited     (TYPE_STRING / TYPE_BYTES)
 *
 * A field in the wire format is: tag varint, then the encoded value.
 * Tag = (field_number << 3) | wire_type.
 */

/**
 * Encode a 64-bit unsigned integer as a protobuf base-128 varint.
 * Returns the number of bytes written (1–10).
 */
static inline int pb_varint(uint8_t *buf, uint64_t val) {
    int n = 0;
    while (val > 0x7F) {
        buf[n++] = (uint8_t)((val & 0x7F) | 0x80);
        val >>= 7;
    }
    buf[n++] = (uint8_t)(val & 0x7F);
    return n;
}

/**
 * transform_record — single-pass JSON → Protobuf → group-split → serialize.
 *
 * Pipeline per call:
 *   1. JSON parse    — scan the 64-field record character by character.
 *   2. Proto encode  — each field encoded directly into protobuf wire format:
 *                        even fields (numeric) → wire type 1 (64-bit LE double)
 *                        odd  fields (string)  → wire type 2 (length-delimited)
 *   3. Group split   — 64 columns partitioned into NUM_GROUPS=4 groups of
 *                      FIELDS_PER_GROUP=16 columns each.  Each group's encoded
 *                      fields are accumulated directly into proto_groups[g],
 *                      so the split happens during encoding with no second pass.
 *   4. Serialized output — proto_groups[g] holds the complete protobuf wire
 *                      message for group g, ready for storage or transport.
 *
 * Protobuf field numbers within each group are 1-based local to that group
 * (field 0 of the JSON record → field 1 of group 0; field 16 → field 1 of
 * group 1; etc.).  A consumer reconstructs the full schema by knowing the
 * group-to-field mapping.
 *
 * The anti-elide byte XORs the first byte of each group output into *sink
 * so the compiler cannot prove the outputs are unused and elide the work.
 *
 * Returns 1 on success, 0 if the JSON record is malformed.
 */
static inline int transform_record(const char * __restrict__ json,
                                    uint8_t proto_groups[NUM_GROUPS][PROTO_GROUP_MAX],
                                    uint8_t * __restrict__ sink) {
    const char *p = json;
    int gpos[NUM_GROUPS] = {0, 0, 0, 0};  /* write cursor per group */

    /* ── 1. Skip to opening brace ───────────────────────────────────── */
    while (*p && *p != '{') p++;
    if (!*p) return 0;
    p++;   /* consume '{' */

    for (int f = 0; f < FIELD_COUNT; f++) {
        /* ── 3. Group assignment ────────────────────────────────────── */
        int g          = f / FIELDS_PER_GROUP;        /* group index 0–3      */
        int local_f    = f % FIELDS_PER_GROUP;        /* position within group */
        int field_num  = local_f + 1;                 /* proto field num (1-based) */
        uint8_t *out   = proto_groups[g];
        int     *opos  = &gpos[g];

        /* Skip ',' between fields */
        if (f > 0) {
            while (*p && *p != ',') p++;
            if (*p) p++;
        }

        /* ── 1. Parse key: "fNN": ───────────────────────────────────── */
        while (*p && *p != '"') p++;   /* opening '"' of key  */
        if (!*p) return 0;
        p++;
        while (*p && *p != '"') p++;   /* key body            */
        if (!*p) return 0;
        p++;                           /* closing '"' of key  */
        while (*p && *p != ':') p++;   /* ':' separator       */
        if (!*p) return 0;
        p++;

        /* ── 2. Proto-encode value ──────────────────────────────────── */
        if (*p == '"') {
            /* Odd field — string: wire type 2 (length-delimited) */
            p++;                           /* consume opening '"'  */
            const char *start = p;
            while (*p && *p != '"') p++;   /* scan value body      */
            int len = (int)(p - start);
            if (*p) p++;                   /* consume closing '"'  */

            *opos += pb_varint(out + *opos, (uint64_t)((field_num << 3) | 2));
            *opos += pb_varint(out + *opos, (uint64_t)len);
            memcpy(out + *opos, start, len);
            *opos += len;
        } else {
            /* Even field — numeric: wire type 1 (64-bit LE IEEE 754 double) */
            char  *endptr = NULL;
            double val    = strtod(p, &endptr);
            if (endptr && endptr > p) p = endptr;

            *opos += pb_varint(out + *opos, (uint64_t)((field_num << 3) | 1));
            memcpy(out + *opos, &val, 8);
            *opos += 8;
        }
    }

    /* ── Anti-elide: XOR first byte of each group output into sink ──── */
    uint8_t s = 0;
    for (int g = 0; g < NUM_GROUPS; g++) {
        if (gpos[g] > 0) s ^= proto_groups[g][0];
    }
    *sink ^= s;
    return 1;
}

/* ── Per-thread state ────────────────────────────────────────────────────── */

/*
 * Fractional-thread period.  200 ms keeps sleep targets >= 20 ms so
 * nanosleep is accurate even on a 10 ms kernel tick.  See the original
 * iBench_cpu.cc for the full overshoot-carry derivation.
 */
#define PERIOD_NS (200000000ULL)   /* 200 ms */

typedef struct {
    int      tid;
    int      cpu_id;      /* -1 = no pinning */
    uint64_t endNs;
    uint64_t intervalNs;  /* 0 = busy-loop (full thread) */
    double   duty_cycle;  /* 1.0 = full; 0 < f < 1 = fractional */
    uint8_t  sink;        /* anti-elide accumulator (written at thread exit) */
    /*
     * ops_count: written after every transform, read lock-free by the reporter.
     * volatile prevents the compiler from caching the value in a register.
     */
    volatile uint64_t ops_count;
} WorkerArgs;

static void *worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

#ifdef __linux__
    if (a->cpu_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET((size_t)a->cpu_id, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
#endif

    /*
     * Private buffers — each worker has its own copy of the JSON record and
     * its own CSV output buffers.  No sharing, no false sharing.
     * Aligned to 64 bytes (cache line) to avoid false sharing on the stack.
     */
    char    __attribute__((aligned(64))) json_in[JSON_MAXLEN];
    /*
     * proto_groups[g] holds the serialized protobuf wire-format message for
     * group g after each transform_record() call.  4 groups × 512 B = 2 KB
     * of output per operation — all on the thread's private stack so there
     * is no cross-thread sharing or false sharing.
     */
    uint8_t __attribute__((aligned(64))) proto_groups[NUM_GROUPS][PROTO_GROUP_MAX];

    build_json(json_in);

    uint64_t ops  = 0;
    uint8_t  sink = 0;

    if (a->duty_cycle < 0.999) {
        /* ── Fractional thread: duty-cycle control with overshoot carry ── */
        /*
         * Each 200 ms period: work for (work_ns_base + carry_ns) ns, then
         * sleep for sleep_ns_base ns.  Any overshoot in the sleep is carried
         * forward as extra work time, so the long-run duty cycle is accurate
         * even when the kernel timer rounds nanosleep up by several ms.
         */
        uint64_t work_ns_base  = (uint64_t)(PERIOD_NS * a->duty_cycle);
        uint64_t sleep_ns_base = PERIOD_NS - work_ns_base;
        uint64_t carry_ns      = 0;

        while (g_keep_running && getNs() < a->endNs) {
            uint64_t phase_end = getNs() + work_ns_base + carry_ns;
            while (g_keep_running && getNs() < phase_end && getNs() < a->endNs) {
                transform_record(json_in, proto_groups, &sink);
                ops++;
                a->ops_count = ops;
            }
            if (sleep_ns_base > 0 && getNs() < a->endNs) {
                uint64_t t0 = getNs();
                sleepNs(sleep_ns_base);
                uint64_t actual = getNs() - t0;
                carry_ns = (actual > sleep_ns_base) ? actual - sleep_ns_base : 0;
            } else {
                carry_ns = 0;
            }
        }
    } else {
        /* ── Full thread: busy-loop or rate-limited ──────────────────── */
        uint64_t nextNs = getNs() + a->intervalNs;

        while (g_keep_running && getNs() < a->endNs) {
            transform_record(json_in, proto_groups, &sink);
            ops++;
            a->ops_count = ops;

            if (a->intervalNs > 0) {
                uint64_t now = getNs();
                if (nextNs > now) sleepNs(nextNs - now);
                now    = getNs();
                nextNs = (nextNs + a->intervalNs > now)
                       ? nextNs + a->intervalNs
                       : now + a->intervalNs;
            }
        }
    }

    a->sink = sink;
    return NULL;
}

/* ── Reporter thread ─────────────────────────────────────────────────────── */

typedef struct {
    WorkerArgs  *workers;
    uint32_t     n_workers;
    int          duration_s;
    volatile int stop;
    /* Filled in before the reporter exits */
    double       mean_ops_s;
    double       stddev_ops_s;
    double       min_ops_s;
    double       max_ops_s;
    int          n_samples;
} ReporterArgs;

static void *reporter_fn(void *arg) {
    ReporterArgs *r = (ReporterArgs *)arg;

    int     max_samples = r->duration_s + 4;
    double *samples     = (double *)calloc((size_t)max_samples, sizeof(double));
    if (!samples) {
        fprintf(stderr, "[reporter] out of memory\n");
        return NULL;
    }

    uint64_t prev_total = 0;
    int n = 0;

    while (!r->stop) {
        sleepNs(NS_PER_S);
        if (r->stop) break;   /* don't record a sample after workers are done */

        /* Lock-free read of total ops across all threads.
         * Minor inaccuracy at 1-second granularity is acceptable. */
        uint64_t total = 0;
        for (uint32_t i = 0; i < r->n_workers; i++)
            total += r->workers[i].ops_count;

        double rate = (double)(total - prev_total);
        prev_total  = total;

        printf("cpu_tput_s%d: %.0f ops/s\n", n, rate);
        fflush(stdout);

        if (n < max_samples)
            samples[n] = rate;
        n++;
    }

    /*
     * Compute summary stats.
     * The Python summariser applies warmup_skip when parsing per-second
     * samples from the log, so the binary's own warmup heuristic here is
     * just a best-effort for the inline summary line.
     * Skip the first sample (warm-up second) when at least 3 samples exist.
     */
    int start = (n >= 3) ? 1 : 0;
    int count = n - start;
    if (count <= 0) {
        r->mean_ops_s   = (n > 0) ? samples[0] : 0.0;
        r->stddev_ops_s = 0.0;
        r->min_ops_s    = r->mean_ops_s;
        r->max_ops_s    = r->mean_ops_s;
        r->n_samples    = n;
        free(samples);
        return NULL;
    }

    double sum = 0.0, mn = 1e18, mx = 0.0;
    for (int i = start; i < n; i++) {
        sum += samples[i];
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    double mean = sum / (double)count;
    double sq   = 0.0;
    for (int i = start; i < n; i++) {
        double d = samples[i] - mean;
        sq += d * d;
    }
    double stddev = (count > 1) ? sqrt(sq / (double)(count - 1)) : 0.0;

    r->mean_ops_s   = mean;
    r->stddev_ops_s = stddev;
    r->min_ops_s    = mn;
    r->max_ops_s    = mx;
    r->n_samples    = n;

    free(samples);
    return NULL;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, const char **argv) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./cpu <duration_s> [n_threads] [rate] [cpu_id]\n"
            "\n"
            "  duration_s   run time in seconds\n"
            "  n_threads    thread count (float); integer part = full 100%%-duty threads,\n"
            "               fractional part = one extra thread at that fraction of a core.\n"
            "               e.g. 2 -> two busy-loop; 2.5 -> two full + one at 50%%;\n"
            "               0.25 -> one thread at 25%% duty. Default: all online CPUs.\n"
            "  rate         transforms/s per full thread (0 = saturate, default).\n"
            "  cpu_id       base CPU for pinning via sched_setaffinity (-1 = no pinning).\n"
            "               Thread i is pinned to cpu_id + i.\n"
            "\n"
            "Work unit per operation (64 columns x 32 B):\n"
            "  1. JSON parse   — scan 64-field record; even=numeric, odd=32-B string.\n"
            "  2. Proto encode — numeric -> wire type 1 (64-bit LE double);\n"
            "                    string  -> wire type 2 (length-delimited).\n"
            "  3. Group split  — 64 columns partitioned into 4 groups of 16 each;\n"
            "                    split happens during encoding (single pass).\n"
            "  4. Serialize    — each group's encoded fields stored as a contiguous\n"
            "                    protobuf wire-format byte string in proto_groups[g].\n"
            "\n"
            "Output: one 'cpu_tput_s<N>: X ops/s' line per second, then:\n"
            "  cpu_throughput mean: X stddev: Y min: Z max: W\n");
        return 1;
    }

    int    durationSec    = atoi(argv[1]);
    long   nproc_         = sysconf(_SC_NPROCESSORS_ONLN);
    double defaultThreads = (nproc_ > 0) ? (double)nproc_ : 1.0;
    double nthreads_f     = (argc >= 3) ? atof(argv[2]) : defaultThreads;

    if (nthreads_f <= 0.0) {
        fprintf(stderr, "ERROR: n_threads must be > 0\n");
        return 1;
    }

    uint32_t fullThreads = (uint32_t)nthreads_f;
    double   fraction    = nthreads_f - (double)fullThreads;
    int      hasFraction = (fraction > 0.005);
    uint32_t numThreads  = fullThreads + (hasFraction ? 1 : 0);
    if (numThreads == 0) {
        fprintf(stderr, "ERROR: n_threads resolves to 0 threads\n");
        return 1;
    }

    uint64_t ratePerThread = (argc >= 4) ? (uint64_t)atoll(argv[3]) : 0;
    int      cpuId         = (argc >= 5) ? atoi(argv[4])            : -1;

    if (durationSec <= 0) {
        fprintf(stderr, "ERROR: duration must be > 0\n");
        return 1;
    }

    uint64_t endNs      = getNs() + (uint64_t)durationSec * NS_PER_S;
    uint64_t intervalNs = (ratePerThread > 0) ? NS_PER_S / ratePerThread : 0;

    /* ── Print banner ────────────────────────────────────────────────── */
    if (hasFraction)
        printf("iBench CPU: %.2f threads (%u full + %.2f fractional) x %ds"
               "  work=JSON-parse->Proto-encode->4-group-split->serialize"
               "  (%d cols x %d B, %d groups x %d cols)\n",
               nthreads_f, fullThreads, fraction, durationSec,
               FIELD_COUNT, STR_VAL_LEN, NUM_GROUPS, FIELDS_PER_GROUP);
    else
        printf("iBench CPU: %u thread(s) x %ds"
               "  work=JSON-parse->Proto-encode->4-group-split->serialize"
               "  (%d cols x %d B, %d groups x %d cols)\n",
               numThreads, durationSec,
               FIELD_COUNT, STR_VAL_LEN, NUM_GROUPS, FIELDS_PER_GROUP);

    if (ratePerThread > 0)
        printf("  rate=%lu transforms/s/thread (full threads only)\n",
               (unsigned long)ratePerThread);
    else
        printf("  rate=saturate (no rate limit)\n");

    if (cpuId >= 0)
        printf("  cpu base=%d (thread i -> cpu %d+i)\n", cpuId, cpuId);
    else
        printf("  cpu=any (no affinity)\n");
    fflush(stdout);

    /* ── Allocate per-thread state ───────────────────────────────────── */
    WorkerArgs *args    = (WorkerArgs *)calloc(numThreads, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(numThreads, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    for (uint32_t i = 0; i < numThreads; i++) {
        int is_frac        = (hasFraction && i == fullThreads);
        args[i].tid        = (int)i;
        args[i].cpu_id     = (cpuId >= 0) ? cpuId + (int)i : -1;
        args[i].endNs      = endNs;
        args[i].intervalNs = is_frac ? 0 : intervalNs;
        args[i].duty_cycle = is_frac ? fraction : 1.0;
        args[i].sink       = 0;
        args[i].ops_count  = 0;
    }

    /* ── Launch reporter thread ──────────────────────────────────────── */
    ReporterArgs reporter_args;
    memset(&reporter_args, 0, sizeof(reporter_args));
    reporter_args.workers    = args;
    reporter_args.n_workers  = numThreads;
    reporter_args.duration_s = durationSec;
    reporter_args.stop       = 0;

    pthread_t reporter_tid;
    if (pthread_create(&reporter_tid, NULL, reporter_fn, &reporter_args) != 0) {
        fprintf(stderr, "ERROR: could not create reporter thread\n");
        return 1;
    }

    /* ── Launch worker threads ───────────────────────────────────────── */
    for (uint32_t i = 0; i < numThreads; i++) {
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
            return 1;
        }
    }

    /* ── Wait for workers ────────────────────────────────────────────── */
    for (uint32_t i = 0; i < numThreads; i++)
        pthread_join(threads[i], NULL);

    /* ── Stop reporter and collect stats ─────────────────────────────── */
    reporter_args.stop = 1;
    pthread_join(reporter_tid, NULL);

    /* ── Tally totals ────────────────────────────────────────────────── */
    uint64_t total_ops  = 0;
    uint8_t  sink_accum = 0;
    for (uint32_t i = 0; i < numThreads; i++) {
        total_ops  += args[i].ops_count;
        sink_accum ^= args[i].sink;
    }
    (void)sink_accum;   /* prevent elision of the worker loops */

    free(args);
    free(threads);

    /* ── Final summary ───────────────────────────────────────────────── */
    printf("iBench CPU: done  threads=%u  duration=%ds  total_ops=%lu\n",
           numThreads, durationSec, (unsigned long)total_ops);

    /*
     * This line is parsed by the cpu_slack_sweep.sh summariser.
     * Format must match: cpu_throughput mean: X stddev: Y min: Z max: W
     * The Python summariser re-derives stats from per-second samples with
     * the experiment's warmup_skip applied, so this line is a cross-check.
     */
    printf("cpu_throughput mean: %.2f stddev: %.2f min: %.2f max: %.2f"
           "  (combined ops/s across all threads, %d 1-s samples)\n",
           reporter_args.mean_ops_s,
           reporter_args.stddev_ops_s,
           reporter_args.min_ops_s,
           reporter_args.max_ops_s,
           reporter_args.n_samples);
    fflush(stdout);

    return 0;
}
