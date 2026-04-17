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
 * IO interference micro-benchmark.
 *
 * Each thread performs sequential writes to a pre-allocated file, wrapping
 * around at max_file_mb to keep disk usage bounded.  Writes use O_DIRECT to
 * bypass the page cache and hit the actual storage device, with automatic
 * fallback to buffered writes + fsync() on filesystems that don't support
 * O_DIRECT.  This matches the design of the Python IO worker embedded in
 * disk_io_slack_sweep.sh and is the C++ replacement for it.
 *
 * Usage:
 *   ./io <duration_s>                                     — 1 thread, saturate, /tmp, 512 MB file
 *   ./io <duration_s> <n_threads>                         — n_threads workers
 *   ./io <duration_s> <n_threads> <rate_ops_per_sec>      — rate-limited ops/s/thread
 *   ./io <duration_s> <n_threads> <rate> <io_size>        — bytes per write (must be ≥ 512, multiple of 512)
 *   ./io <duration_s> <n_threads> <rate> <io_size> <dir>  — write temp files into <dir>
 *   ./io <duration_s> <n_threads> <rate> <io_size> <dir> <max_mb>  — max file size in MiB
 *
 * Parameters:
 *   duration_s       run time in seconds
 *   n_threads        number of worker threads (default: 1)
 *   rate_ops_per_sec target IO ops per second per thread (0 = saturate, default)
 *   io_size          bytes per write (default: 4096; must be a multiple of 512
 *                    for O_DIRECT alignment)
 *   dir              directory for temp files — MUST be on the same disk as the
 *                    RocksDB database so writes create real IO interference.
 *                    Default: /tmp (often tmpfs; use the DB directory instead!)
 *   max_mb           pre-allocate and wrap at this many MiB per thread (default: 512)
 *                    Keeps disk usage bounded regardless of run duration.
 *
 * Design notes:
 *   - Files are pre-allocated with posix_fallocate() so the OS does not need
 *     to allocate blocks on the fly during the benchmark, which would add
 *     metadata overhead and change the IO pattern.
 *   - O_DIRECT bypasses the page cache; fsync() is NOT called in O_DIRECT mode
 *     because writes are already synchronous to the device.  In buffered-IO
 *     fallback mode, fsync() is called after every write.
 *   - posix_memalign() provides a page-aligned buffer required by O_DIRECT.
 *   - Each thread wraps back to offset 0 when it reaches max_mb bytes, so
 *     the file acts as a circular write buffer.
 *   - Temp files are unlinked on clean exit and by the caller's cleanup trap.
 *
 * Compile:
 *   g++ -O2 -pthread -o io iBench_io.cc
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE  /* O_DIRECT, posix_fallocate, posix_memalign */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define NS_PER_S         (1000000000L)
#define DEFAULT_IO_SIZE  (4096)
#define DEFAULT_MAX_MB   (512)
#define DEFAULT_DIR      "/tmp"

/* ── Timing helpers (identical to iBench_cpu.cc) ─────────────────────── */

static inline uint64_t getNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

static inline void sleepNs(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / NS_PER_S);
    ts.tv_nsec = (long  )(ns % NS_PER_S);
    while (nanosleep(&ts, &ts) != 0)
        ;   /* retry remaining time on EINTR */
}

/* ── Per-thread state ─────────────────────────────────────────────────── */
typedef struct {
    int      tid;
    uint64_t endNs;
    uint64_t intervalNs;
    uint32_t io_size;        /* bytes per write()                         */
    uint64_t file_size;      /* pre-allocated file size in bytes           */
    char     path[1024];     /* path of this thread's temp file            */
    uint64_t count;          /* IO operations completed                    */
    uint64_t bytes_written;  /* total bytes written                        */
} WorkerArgs;

/* ── Worker thread ────────────────────────────────────────────────────── */
static void *worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

    /* ── Open temp file ────────────────────────────────────────────────── */
    /* Try O_DIRECT first so writes bypass the page cache and actually hit
     * the storage device.  Some filesystems (tmpfs, older ext3, etc.) don't
     * support O_DIRECT; if open() fails with EINVAL we fall back to buffered
     * writes plus fsync() so at least the data is forced to disk eventually. */
    int use_direct = 1;
    int fd = open(a->path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    if (fd < 0) {
        if (errno == EINVAL || errno == ENOTSUP) {
            /* O_DIRECT not supported on this filesystem — fall back. */
            use_direct = 0;
            fd = open(a->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
        if (fd < 0) {
            fprintf(stderr, "[tid %d] ERROR: open(%s) failed: %s\n",
                    a->tid, a->path, strerror(errno));
            return NULL;
        }
        fprintf(stderr, "[tid %d] WARNING: O_DIRECT not supported on this path; "
                "using buffered writes + fsync() instead.\n", a->tid);
    }

    /* ── Pre-allocate the file ─────────────────────────────────────────── */
    /* posix_fallocate() reserves disk space without writing data, so the OS
     * doesn't need to allocate blocks on the fly during the benchmark.      */
    int fa_ret = posix_fallocate(fd, 0, (off_t)a->file_size);
    if (fa_ret != 0) {
        /* Non-fatal: some filesystems (e.g. tmpfs) don't support fallocate.
         * The file will grow on demand; this changes the IO profile slightly
         * but doesn't break correctness.                                     */
        fprintf(stderr, "[tid %d] WARNING: posix_fallocate failed (%s); "
                "file will grow on demand.\n", a->tid, strerror(fa_ret));
    }

    /* ── Aligned write buffer ──────────────────────────────────────────── */
    /* O_DIRECT requires both the buffer address and transfer length to be
     * aligned to the logical block size (typically 512 B).  We align to the
     * larger of 4096 B (page size) and io_size to be safe.                  */
    size_t   alignment = 4096;
    while (alignment < a->io_size) alignment <<= 1;  /* next power-of-2 ≥ io_size */
    void    *buf_raw   = NULL;
    if (posix_memalign(&buf_raw, alignment, a->io_size) != 0) {
        fprintf(stderr, "[tid %d] ERROR: posix_memalign failed\n", a->tid);
        close(fd);
        unlink(a->path);
        return NULL;
    }
    /* Fill with a non-zero pattern so the OS can't short-circuit the write. */
    memset(buf_raw, 0xAB ^ (unsigned char)a->tid, a->io_size);

    /* ── Main write loop ───────────────────────────────────────────────── */
    uint64_t count   = 0;
    uint64_t bytes   = 0;
    uint64_t pos     = 0;                   /* current file offset in bytes  */
    uint64_t nextNs  = getNs() + a->intervalNs;

    while (getNs() < a->endNs) {
        /* Wrap around: seek back to 0 when the next write would exceed the
         * pre-allocated region.  This keeps file size and disk usage bounded
         * regardless of how long the benchmark runs.                         */
        if (pos + a->io_size > a->file_size) {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                fprintf(stderr, "[tid %d] ERROR: lseek failed: %s\n",
                        a->tid, strerror(errno));
                break;
            }
            pos = 0;
        }

        ssize_t written = write(fd, buf_raw, a->io_size);
        if (written < 0) {
            if (errno == EINTR) continue;   /* interrupted — retry */
            fprintf(stderr, "[tid %d] ERROR: write failed: %s\n",
                    a->tid, strerror(errno));
            break;
        }

        pos   += (uint64_t)written;
        bytes += (uint64_t)written;
        count++;

        /* In buffered-IO fallback mode, fsync() forces data to disk; this is
         * where the actual IO pressure comes from.  In O_DIRECT mode the write
         * is already synchronous to the block layer so fsync is redundant.    */
        if (!use_direct) {
            if (fsync(fd) != 0) {
                fprintf(stderr, "[tid %d] ERROR: fsync failed: %s\n",
                        a->tid, strerror(errno));
                break;
            }
        }

        /* ── Rate limiting (same logic as iBench_cpu.cc) ──────────────── */
        if (a->intervalNs > 0) {
            uint64_t now = getNs();
            if (nextNs > now)
                sleepNs(nextNs - now);
            /* Don't try to catch up if we fell behind. */
            now    = getNs();
            nextNs = (nextNs + a->intervalNs > now)
                   ? nextNs + a->intervalNs
                   : now + a->intervalNs;
        }
    }

    /* ── Cleanup ────────────────────────────────────────────────────────── */
    close(fd);
    unlink(a->path);    /* temp file deleted on clean exit */
    free(buf_raw);

    a->count         = count;
    a->bytes_written = bytes;
    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./io <duration_s> [n_threads] [rate_ops_per_sec] [io_size] [dir] [max_mb]\n"
            "\n"
            "  duration_s       run time in seconds\n"
            "  n_threads        worker threads (default: 1)\n"
            "  rate_ops_per_sec IO ops/sec/thread (0 = saturate, default)\n"
            "  io_size          bytes per write (default: 4096; must be multiple of 512)\n"
            "  dir              directory for temp files (default: /tmp)\n"
            "                   IMPORTANT: use the same directory as the RocksDB database\n"
            "                   so that writes land on the same physical disk.\n"
            "  max_mb           file size limit in MiB per thread (default: 512)\n"
            "                   The file is pre-allocated and writes wrap at this limit\n"
            "                   so disk usage stays bounded regardless of duration.\n");
        return 1;
    }

    int         durationSec   = atoi(argv[1]);
    uint32_t    numThreads    = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 1;
    uint64_t    ratePerThread = (argc >= 4) ? (uint64_t)atoll(argv[3]) : 0;
    uint32_t    io_size       = (argc >= 5) ? (uint32_t)atoi(argv[4]) : DEFAULT_IO_SIZE;
    const char *data_dir      = (argc >= 6) ? argv[5] : DEFAULT_DIR;
    uint64_t    max_mb        = (argc >= 7) ? (uint64_t)atoll(argv[6]) : DEFAULT_MAX_MB;

    if (durationSec <= 0 || numThreads == 0 || io_size == 0) {
        fprintf(stderr, "ERROR: duration, threads, and io_size must be > 0\n");
        return 1;
    }
    if (io_size % 512 != 0) {
        fprintf(stderr, "ERROR: io_size (%u) must be a multiple of 512 (O_DIRECT alignment)\n",
                io_size);
        return 1;
    }
    if (max_mb == 0) {
        fprintf(stderr, "ERROR: max_mb must be > 0\n");
        return 1;
    }

    uint64_t file_size  = max_mb * 1024ULL * 1024ULL;
    uint64_t endNs      = getNs() + (uint64_t)durationSec * NS_PER_S;
    uint64_t intervalNs = (ratePerThread > 0) ? NS_PER_S / ratePerThread : 0;

    printf("iBench IO: %u threads x %ds, %u bytes/op, dir=%s, max_file=%lu MiB",
           numThreads, durationSec, io_size, data_dir, (unsigned long)max_mb);
    if (ratePerThread > 0)
        printf(", rate=%lu ops/s/thread (%.1f MB/s total)",
               (unsigned long)ratePerThread,
               (double)(ratePerThread * numThreads * io_size) / (1024.0 * 1024.0));
    else
        printf(", saturate (unlimited)");
    printf("\n");

    /* ── Launch workers ──────────────────────────────────────────────── */
    WorkerArgs *args    = (WorkerArgs *)calloc(numThreads, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(numThreads, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    /* Build per-thread file paths; include PID so concurrent invocations of
     * this binary don't collide on the same directory.                      */
    pid_t pid = getpid();
    for (uint32_t i = 0; i < numThreads; i++) {
        args[i].tid          = (int)i;
        args[i].endNs        = endNs;
        args[i].intervalNs   = intervalNs;
        args[i].io_size      = io_size;
        args[i].file_size    = file_size;
        args[i].count        = 0;
        args[i].bytes_written= 0;
        snprintf(args[i].path, sizeof(args[i].path),
                 "%s/iBench_io_%d_%u.tmp", data_dir, (int)pid, i);

        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
            return 1;
        }
    }

    for (uint32_t i = 0; i < numThreads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* ── Aggregate and report ────────────────────────────────────────── */
    uint64_t totalOps   = 0;
    uint64_t totalBytes = 0;
    for (uint32_t i = 0; i < numThreads; i++) {
        totalOps   += args[i].count;
        totalBytes += args[i].bytes_written;
    }
    double totalMB = (double)totalBytes / (1024.0 * 1024.0);
    double mbPerSec = totalMB / (durationSec > 0 ? durationSec : 1);

    printf("iBench IO: done\n");
    printf("  total_ops=%lu, bytes=%lu (%.1f MB), throughput=%.1f MB/s\n",
           (unsigned long)totalOps, (unsigned long)totalBytes, totalMB, mbPerSec);

    free(args);
    free(threads);
    return 0;
}
