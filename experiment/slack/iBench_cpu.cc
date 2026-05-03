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
 *   transform_record(json_in, half_a_out, half_b_out)
 *
 *   1. JSON parse  — scan a synthetic 16-field JSON record character by
 *                    character to extract field values.  Alternating fields
 *                    are numeric (double) or alphanumeric string.
 *   2. String→num  — strtod() each numeric field (8 of the 16 fields).
 *   3. CSV format  — snprintf("%.2f") each converted double back to a
 *                    decimal string; memcpy() each string field verbatim.
 *   4. Column split — first 8 fields (CSV) go into half_a, last 8 into half_b,
 *                    mirroring Mycelium's column-split transformation.
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
 * Synthetic JSON record built once per thread, then transformed in a tight
 * loop.  16 fields alternating numeric and string:
 *
 *   {"f00":1000000.000000,"f01":"AAAA...A","f02":1123456.789000,"f03":"CCC...C",...}
 *    ^^^^ even = numeric ^^^^              ^^^^ odd = string ^^^^
 *
 * FIELD_COUNT   total fields
 * STR_VAL_LEN   length of string field values (same fill char, differs per field)
 * JSON_MAXLEN   generous upper bound for the generated JSON string
 * CSV_HALF_MAX  generous upper bound for each CSV half output
 */
#define FIELD_COUNT   16
#define STR_VAL_LEN   80
#define JSON_MAXLEN   2048
#define CSV_HALF_MAX  512

/**
 * Populate `buf` with a synthetic JSON record.
 * Even fields: numeric doubles (varying values).
 * Odd fields:  80-char alphanumeric strings (fill character varies per field).
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

/**
 * Core transformation: JSON → split CSV.
 *
 * Scans the input `json` record field by field.
 *   - Numeric fields: strtod() [string→double], then snprintf("%.2f") [double→string].
 *   - String  fields: raw value copied with memcpy().
 * Fields 0–(FIELD_COUNT/2-1) are written as CSV into half_a.
 * Fields FIELD_COUNT/2–(FIELD_COUNT-1) are written as CSV into half_b.
 *
 * The anti-elide byte is XOR-mixed from the first byte of each half so the
 * compiler cannot prove the outputs are unused and elide the work.
 *
 * Returns 1 on success, 0 if the JSON record is malformed.
 */
static inline int transform_record(const char * __restrict__ json,
                                    char       * __restrict__ half_a,
                                    char       * __restrict__ half_b,
                                    uint8_t    * __restrict__ sink) {
    const char *p = json;
    int pos_a = 0, pos_b = 0;

    /* Skip to opening brace */
    while (*p && *p != '{') p++;
    if (!*p) return 0;
    p++;   /* consume '{' */

    for (int f = 0; f < FIELD_COUNT; f++) {
        /* Select output half and its position counter */
        char *out  = (f < FIELD_COUNT / 2) ? half_a + pos_a : half_b + pos_b;
        int  *opos = (f < FIELD_COUNT / 2) ? &pos_a          : &pos_b;

        /* Skip ',' between fields */
        if (f > 0) {
            while (*p && *p != ',') p++;
            if (*p) p++;  /* consume ',' */
        }

        /* Skip key: "fXX": */
        while (*p && *p != '"') p++;  /* find opening '"' */
        if (!*p) return 0;
        p++;                           /* consume '"' */
        while (*p && *p != '"') p++;  /* skip key characters */
        if (!*p) return 0;
        p++;                           /* consume closing '"' */
        while (*p && *p != ':') p++;  /* find ':' */
        if (!*p) return 0;
        p++;                           /* consume ':' */

        /* Parse value */
        int written;
        if (*p == '"') {
            /* ── String field: copy raw value ──────────────────────── */
            p++;                           /* consume opening '"' */
            const char *start = p;
            while (*p && *p != '"') p++;  /* scan to closing '"' */
            int len = (int)(p - start);
            memcpy(out, start, len);
            written = len;
            if (*p) p++;                  /* consume closing '"' */
        } else {
            /* ── Numeric field: strtod → snprintf ───────────────────── */
            char *endptr = NULL;
            double val   = strtod(p, &endptr);
            if (endptr && endptr > p) p = endptr;
            written = snprintf(out, 32, "%.2f", val);
            /* snprintf writes a NUL at out[written]; it will be overwritten
             * by the comma below (or left as the half terminator). */
        }
        *opos += written;

        /* Comma separator within each half, or NUL terminator at boundary */
        if (f == FIELD_COUNT / 2 - 1) {
            /* End of first half */
            half_a[pos_a] = '\0';
        } else if (f == FIELD_COUNT - 1) {
            /* End of second half */
            half_b[pos_b] = '\0';
        } else {
            out[written]   = ',';
            (*opos)++;
        }
    }

    /* Anti-elide: mix one byte from each output half into the caller's sink */
    *sink ^= (uint8_t)(half_a[0] ^ half_b[0]);
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
    char __attribute__((aligned(64))) json_in[JSON_MAXLEN];
    char __attribute__((aligned(64))) half_a[CSV_HALF_MAX];
    char __attribute__((aligned(64))) half_b[CSV_HALF_MAX];

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
                transform_record(json_in, half_a, half_b, &sink);
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
            transform_record(json_in, half_a, half_b, &sink);
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
            "Work unit: transform_record()\n"
            "  1. Scan a 16-field JSON record (~1 KB) character by character.\n"
            "  2. strtod() the 8 numeric fields (string -> double).\n"
            "  3. snprintf(\"%%,2f\") each double back into CSV (double -> string).\n"
            "  4. memcpy() the 8 string fields verbatim into CSV.\n"
            "  5. Split: fields 0-7 -> half_a CSV, fields 8-15 -> half_b CSV.\n"
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
               "  work=JSON->CSV-split (%d fields, %d numeric + %d string)\n",
               nthreads_f, fullThreads, fraction, durationSec,
               FIELD_COUNT, FIELD_COUNT / 2, FIELD_COUNT / 2);
    else
        printf("iBench CPU: %u thread(s) x %ds"
               "  work=JSON->CSV-split (%d fields, %d numeric + %d string)\n",
               numThreads, durationSec,
               FIELD_COUNT, FIELD_COUNT / 2, FIELD_COUNT / 2);

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
