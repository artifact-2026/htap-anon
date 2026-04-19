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
 * Each thread performs sequential O_DIRECT writes to a pre-allocated file as
 * fast as the storage device allows (no rate limiting, no sleeping).  The
 * write size (io_size) is the single knob that controls how much bandwidth
 * each thread consumes: larger writes transfer more bytes per device round-
 * trip, so bandwidth scales with io_size up to the device's per-writer ceiling.
 *
 * Launching N threads models N independent IO-intensive processes competing
 * for the same disk.  Each thread runs at its own QD=1 saturation rate; the
 * aggregate interference is the sum of all threads' throughputs.
 *
 * Usage:
 *   ./io <duration_s>                          — 1 thread, 64 KB writes, /tmp, 512 MB file
 *   ./io <duration_s> <n_threads>              — n_threads competing writers
 *   ./io <duration_s> <n_threads> <io_size>    — bytes per write (multiple of 512)
 *   ./io <duration_s> <n_threads> <io_size> <dir>      — temp files in <dir>
 *   ./io <duration_s> <n_threads> <io_size> <dir> <max_mb>  — file size cap in MiB
 *
 * Parameters:
 *   duration_s   run time in seconds
 *   n_threads    number of competing writer threads (default: 1)
 *   io_size      bytes per write, must be a multiple of 512 for O_DIRECT
 *                (default: 65536 = 64 KiB — stressful on most NVMe/SSD devices)
 *                Larger values → more bytes per device round-trip → more bandwidth.
 *   dir          directory for temp files; MUST be on the same physical disk as
 *                the RocksDB database so writes create genuine IO interference.
 *                Default: /tmp (often tmpfs — use the DB directory instead!)
 *   max_mb       pre-allocate and wrap at this many MiB per thread (default: 512)
 *                Keeps disk usage bounded regardless of run duration.
 *
 * Design notes:
 *   - No rate limiting: every thread writes as fast as the device allows.
 *     The bandwidth of each QD=1 writer is determined by the device latency
 *     under the concurrent RocksDB load, not by any software throttle.
 *   - io_size is the bandwidth knob.  In the overhead-dominated regime
 *     (io_size small relative to device_rate × fixed_overhead), bandwidth
 *     scales approximately linearly with io_size.  Above that crossover it
 *     asymptotes toward the device's per-writer saturation rate.
 *   - O_DIRECT bypasses the page cache so writes hit the storage device.
 *     Falls back to buffered writes + fsync() on filesystems that don't
 *     support O_DIRECT (e.g. tmpfs, older ext3).
 *   - posix_fallocate() pre-allocates the file so the OS does not need to
 *     allocate blocks on the fly during the run.
 *   - posix_memalign() provides the page-aligned buffer required by O_DIRECT.
 *   - Each thread wraps back to offset 0 when it reaches max_mb, so the
 *     file acts as a circular write buffer and disk usage stays bounded.
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
#define DEFAULT_IO_SIZE  (65536)        /* 64 KiB — stressful on most devices */
#define DEFAULT_MAX_MB   (512)
#define DEFAULT_DIR      "/tmp"

/* ── Timing helper ────────────────────────────────────────────────────── */

static inline uint64_t getNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

/* ── Per-thread state ─────────────────────────────────────────────────── */
typedef struct {
    int      tid;
    uint64_t endNs;
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
     * writes plus fsync() so at least the data is forced to disk.           */
    int use_direct = 1;
    int fd = open(a->path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
    if (fd < 0) {
        if (errno == EINVAL || errno == ENOTSUP) {
            use_direct = 0;
            fd = open(a->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
        if (fd < 0) {
            fprintf(stderr, "[tid %d] ERROR: open(%s) failed: %s\n",
                    a->tid, a->path, strerror(errno));
            return NULL;
        }
        fprintf(stderr, "[tid %d] WARNING: O_DIRECT not supported; "
                "using buffered writes + fsync() instead.\n", a->tid);
    }

    /* ── Pre-allocate the file ─────────────────────────────────────────── */
    int fa_ret = posix_fallocate(fd, 0, (off_t)a->file_size);
    if (fa_ret != 0) {
        fprintf(stderr, "[tid %d] WARNING: posix_fallocate failed (%s); "
                "file will grow on demand.\n", a->tid, strerror(fa_ret));
    }

    /* ── Aligned write buffer ──────────────────────────────────────────── */
    /* O_DIRECT requires the buffer address to be aligned to the logical
     * block size (typically 512 B).  Align to the next power-of-2 ≥ io_size
     * to satisfy both address and length requirements.                       */
    size_t alignment = 4096;
    while (alignment < a->io_size) alignment <<= 1;
    void *buf_raw = NULL;
    if (posix_memalign(&buf_raw, alignment, a->io_size) != 0) {
        fprintf(stderr, "[tid %d] ERROR: posix_memalign failed\n", a->tid);
        close(fd);
        unlink(a->path);
        return NULL;
    }
    /* Non-zero pattern prevents the OS from short-circuiting the write. */
    memset(buf_raw, 0xAB ^ (unsigned char)a->tid, a->io_size);

    /* ── Main write loop (saturate — no rate limiting) ─────────────────── */
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint64_t pos   = 0;   /* current file offset in bytes */

    while (getNs() < a->endNs) {
        /* Wrap: seek back to 0 when the next write would exceed the
         * pre-allocated region, keeping file size and disk usage bounded.   */
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
            if (errno == EINTR) continue;
            fprintf(stderr, "[tid %d] ERROR: write failed: %s\n",
                    a->tid, strerror(errno));
            break;
        }

        pos   += (uint64_t)written;
        bytes += (uint64_t)written;
        count++;

        /* In buffered-IO fallback mode, fsync() is where the actual IO
         * pressure comes from.  O_DIRECT writes are already synchronous to
         * the block layer so fsync is redundant there.                       */
        if (!use_direct) {
            if (fsync(fd) != 0) {
                fprintf(stderr, "[tid %d] ERROR: fsync failed: %s\n",
                        a->tid, strerror(errno));
                break;
            }
        }
    }

    /* ── Cleanup ────────────────────────────────────────────────────────── */
    close(fd);
    unlink(a->path);
    free(buf_raw);

    a->count         = count;
    a->bytes_written = bytes;
    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, const char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./io <duration_s> [n_threads] [io_size] [dir] [max_mb]\n"
            "\n"
            "  duration_s   run time in seconds\n"
            "  n_threads    competing writer threads (default: 1)\n"
            "  io_size      bytes per write, multiple of 512 (default: 65536 = 64 KiB)\n"
            "               Larger → more bytes per device round-trip → more bandwidth.\n"
            "  dir          directory for temp files (default: /tmp)\n"
            "               Use the RocksDB data directory so writes hit the same disk.\n"
            "  max_mb       file size limit in MiB per thread (default: 512)\n");
        return 1;
    }

    int         durationSec = atoi(argv[1]);
    uint32_t    numThreads  = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 1;
    uint32_t    io_size     = (argc >= 4) ? (uint32_t)atoi(argv[3]) : DEFAULT_IO_SIZE;
    const char *data_dir    = (argc >= 5) ? argv[4] : DEFAULT_DIR;
    uint64_t    max_mb      = (argc >= 6) ? (uint64_t)atoll(argv[5]) : DEFAULT_MAX_MB;

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

    uint64_t file_size = max_mb * 1024ULL * 1024ULL;
    uint64_t endNs     = getNs() + (uint64_t)durationSec * NS_PER_S;

    printf("iBench IO: %u thread(s) x %ds, io_size=%u B, dir=%s, max_file=%lu MiB, saturate\n",
           numThreads, durationSec, io_size, data_dir, (unsigned long)max_mb);

    /* ── Launch workers ──────────────────────────────────────────────── */
    WorkerArgs *args    = (WorkerArgs *)calloc(numThreads, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(numThreads, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    /* Include PID in file names so concurrent invocations don't collide. */
    pid_t pid = getpid();
    for (uint32_t i = 0; i < numThreads; i++) {
        args[i].tid          = (int)i;
        args[i].endNs        = endNs;
        args[i].io_size      = io_size;
        args[i].file_size    = file_size;
        args[i].count        = 0;
        args[i].bytes_written = 0;
        snprintf(args[i].path, sizeof(args[i].path),
                 "%s/iBench_io_%d_%u.tmp", data_dir, (int)pid, i);

        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
            return 1;
        }
    }

    for (uint32_t i = 0; i < numThreads; i++)
        pthread_join(threads[i], NULL);

    /* ── Aggregate and report ────────────────────────────────────────── */
    uint64_t totalOps   = 0;
    uint64_t totalBytes = 0;
    for (uint32_t i = 0; i < numThreads; i++) {
        totalOps   += args[i].count;
        totalBytes += args[i].bytes_written;
    }
    double totalMB  = (double)totalBytes / (1024.0 * 1024.0);
    double mbPerSec = totalMB / (durationSec > 0 ? durationSec : 1);

    printf("iBench IO: done\n");
    printf("  total_ops=%lu, bytes=%lu (%.1f MB), throughput=%.1f MB/s\n",
           (unsigned long)totalOps, (unsigned long)totalBytes, totalMB, mbPerSec);

    free(args);
    free(threads);
    return 0;
}
