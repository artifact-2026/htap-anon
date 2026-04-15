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
 *   ./cpu <duration_s> <n_threads> <rate> <rnds> — custom rounds per transform
 *
 * Parameters:
 *   duration_s   run duration in seconds
 *   n_threads    number of worker threads (default: all cores)
 *   rate         target transforms per second per thread (0 = no limit, default)
 *   rnds         arithmetic rounds per transform (default: 512)
 *
 * When rate > 0 each thread sleeps between transforms (clock_nanosleep) to
 * hit the target rate.  When rate = 0, threads busy-loop (100 % duty cycle).
 *
 * Compile:
 *   g++ -O2 -fopenmp -o cpu iBench_cpu.cc
 *   # or: g++ -O2 -pthread -fopenmp -o cpu iBench_cpu.cc -lrt
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>

#define NS_PER_S  (1000000000L)

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
 * Sleep until the target wall-clock nanosecond.
 * Uses CLOCK_MONOTONIC + TIMER_ABSTIME for jitter-free scheduling.
 */
static inline void sleepUntilNs(uint64_t targetNs) {
    struct timespec ts;
    ts.tv_sec  = targetNs / NS_PER_S;
    ts.tv_nsec = targetNs % NS_PER_S;
    /* Loop on EINTR. */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0)
        ;
}

int main(int argc, const char** argv) {
    /* ── Parse arguments ─────────────────────────────────────────────── */
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./cpu <duration_s> [n_threads] [rate_per_thread] [rounds]\n"
            "\n"
            "  duration_s   run time in seconds\n"
            "  n_threads    worker threads (default: all cores)\n"
            "  rate         transforms/sec/thread (0 = saturate, default)\n"
            "  rounds       Murmur3 mix rounds per transform (default: 512)\n");
        return 1;
    }

    int      durationSec   = atoi(argv[1]);
    uint32_t numThreads    = (argc >= 3) ? (uint32_t)atoi(argv[2])
                                         : (uint32_t)omp_get_num_procs();
    uint64_t ratePerThread = (argc >= 4) ? (uint64_t)atoll(argv[3]) : 0;
    uint32_t rounds        = (argc >= 5) ? (uint32_t)atoi(argv[4])  : 512;

    if (durationSec <= 0 || numThreads == 0 || rounds == 0) {
        fprintf(stderr, "ERROR: duration, threads, and rounds must be > 0\n");
        return 1;
    }

    uint64_t endNs      = getNs() + (uint64_t)durationSec * NS_PER_S;
    uint64_t intervalNs = (ratePerThread > 0) ? NS_PER_S / ratePerThread : 0;

    printf("iBench CPU: %d threads × %ds, %u rounds/transform",
           numThreads, durationSec, rounds);
    if (ratePerThread > 0)
        printf(", rate=%lu transforms/s/thread", (unsigned long)ratePerThread);
    else
        printf(", saturate (no rate limit)");
    printf("\n");

    /* ── Launch workers ──────────────────────────────────────────────── */
    omp_set_num_threads(numThreads);

    /*
     * volatile prevents the compiler from optimising away the entire
     * mix chain (the result is "used" by writing to this sink).
     */
    volatile uint64_t sink = 0;

#pragma omp parallel
    {
        int tid = omp_get_thread_num();
        /* Per-thread state seeded by thread ID × golden ratio. */
        uint64_t state = (uint64_t)tid * 0x9E3779B97F4A7C15ULL;
        uint64_t nextNs = getNs() + intervalNs;

        while (getNs() < endNs) {
            /* ── one transform: `rounds` mix iterations ────────────── */
            uint64_t x = state;
            for (uint32_t r = 0; r < rounds; r++) {
                x = mix64(x);
            }
            state = x;   /* data-dependent: feeds back into next transform */

            /* ── rate limiting ─────────────────────────────────────── */
            if (intervalNs > 0) {
                uint64_t now = getNs();
                if (nextNs > now) {
                    sleepUntilNs(nextNs);
                }
                /* Don't try to catch up if we fell behind. */
                now = getNs();
                nextNs = (nextNs + intervalNs > now)
                       ? nextNs + intervalNs
                       : now + intervalNs;
            }
        }

        /* Prevent the compiler from eliding the entire loop. */
        sink = state;
    }

    (void)sink;
    printf("iBench CPU: done\n");
    return 0;
}