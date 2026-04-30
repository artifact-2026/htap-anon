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
 * CPU interference micro-benchmark.
 *
 * Each thread performs Murmur3-style 64-bit integer mix rounds — pure
 * register arithmetic with a data-dependent chain that prevents the
 * compiler from optimising it away.  This matches the transform-worker
 * model used in the HTAP cpu_slack_sweep.sh experiment.
 *
 * Usage:
 *   ./cpu <duration_s>                           — saturate all cores (100% duty)
 *   ./cpu <duration_s> <n_threads>               — saturate n_threads cores
 *   ./cpu <duration_s> <n_threads> <rate>        — rate-limited transforms/s/thread
 *   ./cpu <duration_s> <n_threads> <rate> <rnds> <cpu_id> — pin threads starting at CPU cpu_id
 *
 * Parameters:
 *   duration_s   run duration in seconds
 *   n_threads    worker thread count — accepts a float such as 1.5, where the integer
 *                part (1) is the number of full 100%-duty-cycle threads and the fractional
 *                part (0.5) spawns one additional thread running at that fraction of one core.
 *                The fractional thread alternates between work and sleep in 50 ms periods
 *                (work_ms = fraction × 50, sleep_ms = (1−fraction) × 50) which keeps the
 *                sleep large enough for accurate nanosleep control even at small fractions.
 *                Default: all cores.  Examples: 2 → two full threads; 2.5 → two full + one
 *                at 50% duty; 0.25 → one thread at 25% duty (no full threads).
 *   rate         target transforms per second per FULL thread (0 = no limit, default)
 *                Ignored for the fractional thread, which uses duty-cycle control instead.
 *   rnds         arithmetic rounds per transform (default: 512)
 *   cpu_id       Base CPU for per-thread affinity via sched_setaffinity (-1 = no pinning,
 *                default).  Thread i is pinned to cpu_id + i, so a 1.5-thread invocation
 *                pins the full thread to cpu_id and the fractional thread to cpu_id + 1.
 *               Pinning ensures each iBench process truly occupies its core and is not
 *               time-sliced with YCSB threads by the CFS scheduler.  Without pinning,
 *               CFS awards YCSB threads (which block on I/O) priority boosts each time
 *               they unblock, so adding more busy-loop workers can paradoxically leave
 *               YCSB throughput flat or even increase it.
 *
 * When rate > 0 each thread sleeps between transforms (clock_nanosleep) to
 * hit the target rate.  When rate = 0, threads busy-loop (100 % duty cycle).
 *
 * Compile:
 *   g++ -O2 -pthread -o cpu iBench_cpu.cc -lrt
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>   /* sysconf */
#include <signal.h>   /* signal handling */
#ifdef __linux__
#  include <sched.h>  /* sched_setaffinity, CPU_SET */
#endif

volatile sig_atomic_t g_keep_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_keep_running = 0;
}

#define NS_PER_S   (1000000000L)

/* Work/sleep period for fractional threads.
 * The fractional thread runs transforms for (duty_cycle × PERIOD_NS) ns then
 * sleeps for ((1 − duty_cycle) × PERIOD_NS) ns.
 *
 * Period choice: nanosleep has a timer-resolution floor of ~1–10 ms on Linux
 * (depending on CONFIG_HZ and tickless settings).  A 50 ms period gives only
 * 25 ms of sleep for a 50% fractional thread, which a 10 ms tick rounds to
 * 30–40 ms — a 20–60% overshoot that lowers the achieved duty cycle.
 * 200 ms gives 100 ms of sleep for the same fraction: even a 10 ms tick
 * introduces at most 10% relative error, and at 5% fraction the sleep is
 * still 190 ms — accurate on any hardware.  The coarser period (200 ms)
 * is irrelevant for experiments that run for hundreds of seconds.           */
#define PERIOD_NS  (200000000ULL)  /* 200 ms */

/* Murmur3 finalisation constants. */
static const uint64_t M1   = 0xFF51AFD7ED558CCDULL;
static const uint64_t M2   = 0xC4CEB9FE1A85EC53ULL;

/**
 * One Murmur3-style 64-bit integer finalisation round.
 * Pure register work — no memory access, no branch.
 * The data-dependent chain (output feeds back in) prevents
 * the compiler from eliding the computation.
 */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;  x *= M1;
    x ^= x >> 33;  x *= M2;
    x ^= x >> 33;
    return x;
}

static inline uint64_t getNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

/**
 * Sleep for approximately 'ns' nanoseconds.
 * Uses nanosleep (portable: Linux + macOS). On EINTR, retries with the
 * remaining time so the total sleep is at least 'ns' nanoseconds.
 */
static inline void sleepNs(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long)(ns % NS_PER_S);
    while (nanosleep(&ts, &ts) != 0)
        ;   /* retry remainder on EINTR */
}

/* ── Per-thread state passed through pthread_create ───────────────────── */
typedef struct {
    int      tid;
    int      cpu_id;    /* >= 0: pin this thread to that CPU via sched_setaffinity;
                           -1  = no pinning (OS schedules freely).             */
    uint64_t endNs;
    uint64_t intervalNs; /* 0 = busy-loop (full thread); > 0 = rate-limited    */
    double   duty_cycle; /* 1.0 = full thread; 0 < f < 1 = fractional thread.
                           Fractional threads alternate work/sleep in PERIOD_NS
                           windows instead of using intervalNs.                */
    uint32_t rounds;
    uint64_t sink_out;  /* anti-elide: worker writes final state here (per-thread,
                           no sharing — eliminates the data race on a single shared
                           volatile sink that the original design had).         */
    uint64_t count;     /* transforms completed by this thread */
} WorkerArgs;

static void *worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

    /* Optional CPU affinity — pin this thread to a single core so the iBench
     * process genuinely occupies that core's capacity.  Without pinning, the
     * Linux CFS scheduler awards YCSB threads (which sleep on I/O) a "sleeper
     * fairness" bonus each time they unblock, so they can end up with more CPU
     * than their fair share even as iBench workers pile up.  Pinning removes
     * that scheduling asymmetry and makes the interference deterministic.    */
#ifdef __linux__
    if (a->cpu_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET((size_t)a->cpu_id, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
#endif

    uint64_t state = (uint64_t)a->tid * 0x9E3779B97F4A7C15ULL;
    uint64_t count = 0;

    if (a->duty_cycle < 0.999) {
        /* ── Fractional thread: work/sleep with overshoot compensation ── */
        /*
         * Each period: work for (work_ns_base + carry_ns) ns, then sleep
         * for sleep_ns_base ns.  If the sleep overshoots the target (because
         * the kernel's timer resolution rounds nanosleep up), the surplus is
         * carried into the next work phase as extra wall time.
         *
         * Steady-state analysis — suppose actual_sleep = sleep_ns_base + D:
         *   carry stabilises at D.
         *   work phase = work_ns_base + D
         *   actual duty = (work_ns_base + D) / (work_ns_base + D + sleep_ns_base + D)
         *                = (work_ns_base + D) / (PERIOD_NS + 2D)
         *
         * When D is small relative to PERIOD_NS (e.g. 10 ms against 200 ms),
         * the achieved duty cycle converges to within ~D/PERIOD_NS of the
         * target.  More importantly, the cumulative work fraction over many
         * periods is exact: every ns of sleep overshoot is repaid as ns of
         * extra work in the next period.
         *
         * Why not sleep per-transform?  A transform at 4096 rounds ≈ 10–50 µs.
         * nanosleep on a 10 ms-tick kernel has a minimum effective sleep of
         * ~10–20 ms, so per-transform sleeping achieves < 1% of the requested
         * duty cycle.  PERIOD_NS = 200 ms keeps sleep targets ≥ 20 ms, which
         * is resolved accurately on any Linux system.
         */
        uint64_t work_ns_base  = (uint64_t)(PERIOD_NS * a->duty_cycle);
        uint64_t sleep_ns_base = PERIOD_NS - work_ns_base;
        uint64_t carry_ns      = 0;   /* sleep overshoot → extra work next period */

        while (g_keep_running && getNs() < a->endNs) {
            /* Work phase: run until work_ns_base + carry_ns elapses */
            uint64_t phase_end = getNs() + work_ns_base + carry_ns;
            while (g_keep_running && getNs() < phase_end && getNs() < a->endNs) {
                uint64_t x = state;
                for (uint32_t r = 0; r < a->rounds; r++) x = mix64(x);
                state = x;
                count++;
            }
            /* Sleep phase: measure actual duration and compute overshoot */
            if (sleep_ns_base > 0 && getNs() < a->endNs) {
                uint64_t t0 = getNs();
                sleepNs(sleep_ns_base);
                uint64_t actual_sleep = getNs() - t0;
                carry_ns = (actual_sleep > sleep_ns_base)
                         ? actual_sleep - sleep_ns_base : 0;
            } else {
                carry_ns = 0;
            }
        }
    } else {
        /* ── Full thread: busy-loop or rate-limited ─────────────────── */
        uint64_t nextNs = getNs() + a->intervalNs;

        while (g_keep_running && getNs() < a->endNs) {
            uint64_t x = state;
            for (uint32_t r = 0; r < a->rounds; r++) x = mix64(x);
            state = x;
            count++;

            if (a->intervalNs > 0) {
                uint64_t now = getNs();
                if (nextNs > now) sleepNs(nextNs - now);
                /* Don't try to catch up if we fell behind. */
                now    = getNs();
                nextNs = (nextNs + a->intervalNs > now)
                       ? nextNs + a->intervalNs
                       : now + a->intervalNs;
            }
        }
    }

    /* Prevent the compiler from eliding the entire loop. Written to a
     * per-thread field (not a shared pointer) to avoid the data race the
     * original single shared-sink design had.                             */
    a->sink_out = state;
    a->count    = count;
    return NULL;
}

int main(int argc, const char** argv) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    /* ── Parse arguments ─────────────────────────────────────────────── */
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./cpu <duration_s> [n_threads] [rate_per_thread] [rounds] [cpu_id]\n"
            "\n"
            "  duration_s   run time in seconds\n"
            "  n_threads    thread count (float); integer part = full 100%%-duty threads,\n"
            "               fractional part = one extra thread at that fraction of a core.\n"
            "               e.g. 2 → two busy-loop threads; 2.5 → two full + one at 50%%;\n"
            "               0.25 → one thread at 25%% duty (no full threads).\n"
            "               Default: all online CPUs.\n"
            "  rate         transforms/sec/thread for FULL threads (0 = saturate, default)\n"
            "               The fractional thread uses duty-cycle control, not this rate.\n"
            "  rounds       Murmur3 mix rounds per transform (default: 512)\n"
            "  cpu_id       Base CPU for pinning via sched_setaffinity (-1 = no pinning).\n"
            "               Thread i is pinned to cpu_id + i, so a 1.5-thread invocation\n"
            "               uses cpu_id for the full thread and cpu_id+1 for the fractional.\n");
        return 1;
    }

    int    durationSec   = atoi(argv[1]);
    long   nproc_        = sysconf(_SC_NPROCESSORS_ONLN);
    double defaultThreads = (nproc_ > 0) ? (double)nproc_ : 1.0;
    double nthreads_f    = (argc >= 3) ? atof(argv[2]) : defaultThreads;

    if (nthreads_f <= 0.0) {
        fprintf(stderr, "ERROR: n_threads must be > 0\n");
        return 1;
    }

    uint32_t fullThreads = (uint32_t)nthreads_f;
    double   fraction    = nthreads_f - (double)fullThreads;
    int      hasFraction = (fraction > 0.005); /* guard against float rounding noise */
    uint32_t numThreads  = fullThreads + (hasFraction ? 1 : 0);
    if (numThreads == 0) {
        fprintf(stderr, "ERROR: n_threads resolves to 0 threads\n");
        return 1;
    }

    uint64_t ratePerThread = (argc >= 4) ? (uint64_t)atoll(argv[3]) : 0;
    uint32_t rounds        = (argc >= 5) ? (uint32_t)atoi(argv[4])  : 512;
    int      cpuId         = (argc >= 6) ? atoi(argv[5])            : -1;

    if (durationSec <= 0 || rounds == 0) {
        fprintf(stderr, "ERROR: duration and rounds must be > 0\n");
        return 1;
    }

    uint64_t endNs      = getNs() + (uint64_t)durationSec * NS_PER_S;
    uint64_t intervalNs = (ratePerThread > 0) ? NS_PER_S / ratePerThread : 0;

    if (hasFraction)
        printf("iBench CPU: %.2f threads (%u full + %.2f fractional) x %ds, %u rounds/transform",
               nthreads_f, fullThreads, fraction, durationSec, rounds);
    else
        printf("iBench CPU: %u threads x %ds, %u rounds/transform",
               numThreads, durationSec, rounds);
    if (ratePerThread > 0)
        printf(", rate=%lu transforms/s/thread (full threads only)",
               (unsigned long)ratePerThread);
    else
        printf(", saturate (no rate limit)");
    if (hasFraction)
        printf(", fractional duty=%.1f%% (%.0fms work / %.0fms sleep per 200ms period)",
               fraction * 100.0,
               fraction * 200.0,
               (1.0 - fraction) * 200.0);
    if (cpuId >= 0)
        printf(", cpu base=%d (thread i → cpu %d+i)", cpuId, cpuId);
    else
        printf(", cpu=any (no affinity)");
    printf("\n");

    /* ── Launch workers ──────────────────────────────────────────────── */
    WorkerArgs *args    = (WorkerArgs *)calloc(numThreads, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(numThreads, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    for (uint32_t i = 0; i < numThreads; i++) {
        int is_fractional   = (hasFraction && i == fullThreads);
        args[i].tid         = (int)i;
        /* cpu_id is now a per-thread base: thread i → cpuId + i.
         * For single-thread invocations (the common shell-script case) this
         * is identical to the old "pin everything to cpuId" behaviour.      */
        args[i].cpu_id      = (cpuId >= 0) ? cpuId + (int)i : -1;
        args[i].endNs       = endNs;
        args[i].intervalNs  = is_fractional ? 0 : intervalNs;
        args[i].duty_cycle  = is_fractional ? fraction : 1.0;
        args[i].rounds      = rounds;
        args[i].sink_out    = 0;
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
            return 1;
        }
    }

    for (uint32_t i = 0; i < numThreads; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t totalTransforms = 0;
    /* Anti-elide: XOR all per-thread sinks after all joins — the compiler
     * cannot prove the values are unused so it cannot elide the worker loops.
     * Using per-thread sink_out avoids the data race the old shared-pointer
     * design had when multiple threads wrote *sink simultaneously.            */
    uint64_t sink_accum = 0;
    for (uint32_t i = 0; i < numThreads; i++) {
        totalTransforms += args[i].count;
        sink_accum      ^= args[i].sink_out;
    }

    free(args);
    free(threads);

    (void)sink_accum;
    printf("iBench CPU: done  transforms=%lu  threads=%u  duration=%ds\n",
           (unsigned long)totalTransforms, numThreads, durationSec);
    return 0;
}
