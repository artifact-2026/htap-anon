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
 * IO interference micro-benchmark — read+write competitor units.
 *
 * Each competitor unit spawns one write thread and one read thread:
 *
 *   Write thread — sequential O_DIRECT writes advancing through a large
 *                  pre-allocated file, wrapping at EOF.  Default 4 KiB block
 *                  size matches RocksDB's default SST block read size, so
 *                  writes stress the same I/O queue depth as compaction and
 *                  compete for write IOPS with YCSB.
 *
 *   Read thread  — O_DIRECT reads with RANDOM offsets into a large
 *                  pre-initialised file.  Random access mirrors YCSB's cache-
 *                  missing point-lookup pattern, defeating the SSD's sequential
 *                  read-ahead cache.  Each 4 KiB block read is a genuine NAND
 *                  access that competes directly with YCSB's disk reads.
 *
 * File sizes are large (default 4 GiB per thread) to overflow the SSD's
 * internal SLC write-back cache, ensuring every I/O reaches real NAND rather
 * than being absorbed by the drive's fast tier.
 *
 * Two-pass mode (recommended for large files):
 *   Pass 1  --init-only    Only initialise read files (buffered writes, not
 *                          timed).  Write files are not created yet.  Run this
 *                          before YCSB starts and then drop page cache.
 *   Pass 2  --skip-init    Start with YCSB running.  Read threads open the
 *                          already-initialised files from Pass 1 and start
 *                          random-read immediately.  Write threads create fresh
 *                          files.  Both run for the full RUNTIME_SECS window.
 *
 *   File naming is PID-free in two-pass mode so both passes use the same paths:
 *     <dir>/iBench_io_r_<N>.tmp  (N = thread index, 0-based)
 *     <dir>/iBench_io_w_<N>.tmp
 *
 * Normal (single-pass) mode:
 *   PID is embedded in filenames to allow concurrent invocations.  Read files
 *   are initialised at thread startup (may overlap with YCSB warm-up).
 *
 * Usage:
 *   ./io <duration_s> [n_workers] [io_size] [dir] [max_mb] [--init-only] [--skip-init]
 *
 * Parameters:
 *   duration_s  run time in seconds (ignored for --init-only; init runs to completion)
 *   n_workers   competitor units; spawns n_workers write + n_workers read threads
 *               (--init-only only launches read threads)
 *   io_size     bytes per read or write op; multiple of 512 for O_DIRECT (default: 4 KiB)
 *   dir         directory for temp files — MUST be on the same physical disk as RocksDB
 *   max_mb      file size per thread in MiB (default: 4096 = 4 GiB)
 *
 * Compile:
 *   g++ -O2 -pthread -o io iBench_io.cc
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE   /* O_DIRECT, posix_fallocate, posix_memalign */
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

volatile sig_atomic_t g_keep_running = 1;

static void handle_sig(int sig) {
    (void)sig;
    g_keep_running = 0;
}
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define NS_PER_S        (1000000000L)
#define DEFAULT_IO_SIZE (4 * 1024)           /* 4 KiB — matches RocksDB's default SST block read size */
#define DEFAULT_MAX_MB  (4096)               /* 4 GiB — overflows typical SSD SLC cache    */
#define DEFAULT_DIR     "/tmp"

/* ── Timing ───────────────────────────────────────────────────────────── */

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
        ;   /* retry remainder on EINTR */
}

/* ── Per-thread state ─────────────────────────────────────────────────── */

typedef struct {
    int      tid;
    int      is_writer;     /* 1 = write thread, 0 = read thread              */
    int      skip_init;     /* read thread: skip Phase 1 file initialisation   */
    int      init_only;     /* read thread: do Phase 1 only, skip timed loop   */
    int      keep_file;     /* do not unlink file after thread exits            */
    uint64_t endNs;
    uint64_t intervalNs;    /* sleep interval between ops to maintain target rate */
    uint32_t io_size;       /* bytes per op; multiple of 512                   */
    uint64_t file_size;     /* pre-allocated size; multiple of io_size          */
    char     path[1024];
    uint64_t count;         /* ops completed in the timed window (written at exit) */
    /*
     * ops_xfered: incremented inside every op (one write() or pread() = 1 op)
     * so the reporter thread can compute ops/s each second.  volatile prevents
     * the compiler from caching the value in a register between the worker
     * write and the reporter read.  Minor data races at 1-second granularity
     * are fine — the reporter only needs an approximate snapshot.
     */
    volatile uint64_t ops_xfered;
} WorkerArgs;

/* ── Reporter thread ─────────────────────────────────────────────────────── */

typedef struct {
    WorkerArgs  *workers;
    uint32_t     n_workers;   /* competitor units; write threads = [0,n), read = [n,2n) */
    uint32_t     io_size;     /* bytes per op — used to report MB/s in final summary    */
    int          duration_s;
    volatile int stop;
    /* Summary stats filled in before the reporter exits (total ops/s) */
    double       total_mean_ops;
    double       total_std_ops;
    double       total_min_ops;
    double       total_max_ops;
    double       write_mean_ops;
    double       read_mean_ops;
    int          n_samples;
} ReporterArgs;

static void *reporter_fn(void *arg) {
    ReporterArgs *r = (ReporterArgs *)arg;

    int     max_samples  = r->duration_s + 4;
    double *total_samp   = (double *)calloc((size_t)max_samples, sizeof(double));
    double *write_samp   = (double *)calloc((size_t)max_samples, sizeof(double));
    double *read_samp    = (double *)calloc((size_t)max_samples, sizeof(double));
    if (!total_samp || !write_samp || !read_samp) {
        fprintf(stderr, "[io reporter] out of memory\n");
        free(total_samp); free(write_samp); free(read_samp);
        return NULL;
    }

    uint64_t prev_write = 0, prev_read = 0;
    int n = 0;

    while (!r->stop) {
        struct timespec ts = { 1, 0 };
        while (nanosleep(&ts, &ts) != 0) ;
        if (r->stop) break;

        /* Lock-free snapshot of per-thread op counters. */
        uint64_t write_ops = 0, read_ops = 0;
        for (uint32_t i = 0; i < r->n_workers * 2; i++) {
            if (r->workers[i].is_writer)
                write_ops += r->workers[i].ops_xfered;
            else
                read_ops  += r->workers[i].ops_xfered;
        }

        double wops = (double)(write_ops - prev_write);
        double rops = (double)(read_ops  - prev_read);
        double tops = wops + rops;
        prev_write = write_ops;
        prev_read  = read_ops;

        printf("io_tput_s%d: %.1f write_ops/s %.1f read_ops/s %.1f total_ops/s\n",
               n, wops, rops, tops);
        fflush(stdout);

        if (n < max_samples) {
            total_samp[n] = tops;
            write_samp[n] = wops;
            read_samp[n]  = rops;
        }
        n++;
    }

    /* Compute stats over steady-state samples (skip first as warmup). */
    int start = (n >= 3) ? 1 : 0;
    int count = n - start;
    if (count <= 0) {
        r->total_mean_ops = (n > 0) ? total_samp[0] : 0.0;
        r->total_std_ops  = 0.0;
        r->total_min_ops  = r->total_mean_ops;
        r->total_max_ops  = r->total_mean_ops;
        r->write_mean_ops = (n > 0) ? write_samp[0] : 0.0;
        r->read_mean_ops  = (n > 0) ? read_samp[0]  : 0.0;
        r->n_samples = n;
        free(total_samp); free(write_samp); free(read_samp);
        return NULL;
    }

    double tsum = 0.0, tmn = 1e18, tmx = 0.0;
    double wsum = 0.0, rsum = 0.0;
    for (int i = start; i < n; i++) {
        tsum += total_samp[i];
        wsum += write_samp[i];
        rsum += read_samp[i];
        if (total_samp[i] < tmn) tmn = total_samp[i];
        if (total_samp[i] > tmx) tmx = total_samp[i];
    }
    double tmean = tsum / (double)count;
    double sq = 0.0;
    for (int i = start; i < n; i++) {
        double d = total_samp[i] - tmean;
        sq += d * d;
    }
    double tstd = (count > 1) ? sqrt(sq / (double)(count - 1)) : 0.0;

    r->total_mean_ops = tmean;
    r->total_std_ops  = tstd;
    r->total_min_ops  = tmn;
    r->total_max_ops  = tmx;
    r->write_mean_ops = wsum / (double)count;
    r->read_mean_ops  = rsum / (double)count;
    r->n_samples = n;

    free(total_samp); free(write_samp); free(read_samp);
    return NULL;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Try to open with O_DIRECT; fall back to buffered on unsupported fs. */
static int open_direct(const char *path, int base_flags, int *out_direct) {
    int fd = open(path, base_flags | O_DIRECT, 0600);
    if (fd >= 0) { *out_direct = 1; return fd; }
    if (errno == EINVAL || errno == ENOTSUP) {
        fd = open(path, base_flags, 0600);
        *out_direct = 0;
        return fd;
    }
    return -1;  /* genuine error */
}

/* Allocate a buffer aligned to the next power-of-2 >= io_size (required by
 * O_DIRECT for both address and transfer-length alignment).               */
static void *alloc_aligned(uint32_t io_size, unsigned char fill) {
    size_t alignment = 4096;
    while (alignment < io_size) alignment <<= 1;
    void *buf = NULL;
    if (posix_memalign(&buf, alignment, io_size) != 0) return NULL;
    memset(buf, fill, io_size);
    return buf;
}

/* ── Write worker ─────────────────────────────────────────────────────── */

static void *write_worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

    int use_direct;
    int fd = open_direct(a->path, O_WRONLY | O_CREAT | O_TRUNC, &use_direct);
    if (fd < 0) {
        fprintf(stderr, "[write %d] ERROR: open(%s): %s\n",
                a->tid, a->path, strerror(errno));
        return NULL;
    }
    if (!use_direct)
        fprintf(stderr, "[write %d] WARNING: O_DIRECT not supported; "
                "using buffered writes + fsync.\n", a->tid);

    int fa = posix_fallocate(fd, 0, (off_t)a->file_size);
    if (fa != 0)
        fprintf(stderr, "[write %d] WARNING: posix_fallocate: %s; "
                "file will grow on demand.\n", a->tid, strerror(fa));

    void *buf = alloc_aligned(a->io_size, 0xAB ^ (unsigned char)a->tid);
    if (!buf) {
        fprintf(stderr, "[write %d] ERROR: posix_memalign failed\n", a->tid);
        close(fd);
        if (!a->keep_file) unlink(a->path);
        return NULL;
    }

    uint64_t count = 0, bytes = 0, pos = 0;
    uint64_t nextNs = getNs() + a->intervalNs;
    while (g_keep_running && getNs() < a->endNs) {
        if (pos + a->io_size > a->file_size) {
            if (lseek(fd, 0, SEEK_SET) < 0) {
                fprintf(stderr, "[write %d] ERROR: lseek: %s\n",
                        a->tid, strerror(errno));
                break;
            }
            pos = 0;
        }
        ssize_t n = write(fd, buf, a->io_size);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[write %d] ERROR: write: %s\n",
                    a->tid, strerror(errno));
            break;
        }
        pos += (uint64_t)n; bytes += (uint64_t)n; count++;
        a->ops_xfered = count;   /* reporter reads this lock-free each second */
        if (!use_direct && fsync(fd) != 0) {
            fprintf(stderr, "[write %d] ERROR: fsync: %s\n",
                    a->tid, strerror(errno));
            break;
        }

        if (a->intervalNs > 0) {
            uint64_t now = getNs();
            if (nextNs > now) sleepNs(nextNs - now);
            now = getNs();
            nextNs = (nextNs + a->intervalNs > now) ? nextNs + a->intervalNs : now + a->intervalNs;
        }
    }

    close(fd);
    if (!a->keep_file) unlink(a->path);
    free(buf);
    a->count = count;
    return NULL;
}

/* ── Read worker ──────────────────────────────────────────────────────── */

static void *read_worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;

    /* ── Phase 1: write-initialise the file (buffered, not timed) ────────
     *
     * O_DIRECT reads from a posix_fallocate'd-but-never-written file may
     * trigger on-demand kernel zero-fill writes, polluting the write metrics.
     * A one-time buffered sequential write commits extents cleanly.
     *
     * In two-pass mode (--init-only), this is the entire job: write the file
     * and return.  The file is left on disk for Pass 2 (--skip-init).
     * In --skip-init mode this phase is skipped entirely.                   */
    if (!a->skip_init) {
        int fd_init = open(a->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd_init < 0) {
            fprintf(stderr, "[read %d] ERROR: init open(%s): %s\n",
                    a->tid, a->path, strerror(errno));
            return NULL;
        }
        void *ibuf = malloc(a->io_size);
        if (!ibuf) {
            fprintf(stderr, "[read %d] ERROR: malloc for init failed\n", a->tid);
            close(fd_init);
            if (!a->keep_file) unlink(a->path);
            return NULL;
        }
        memset(ibuf, 0xCD ^ (unsigned char)a->tid, a->io_size);

        uint64_t written = 0;
        while (written < a->file_size) {
            ssize_t n = write(fd_init, ibuf, a->io_size);
            if (n <= 0) {
                fprintf(stderr, "[read %d] WARNING: init write truncated "
                        "at %lu/%lu bytes\n",
                        a->tid, (unsigned long)written,
                        (unsigned long)a->file_size);
                break;
            }
            written += (uint64_t)n;
        }
        fsync(fd_init);
        close(fd_init);
        free(ibuf);

        if (a->init_only) {
            /* Two-pass mode Pass 1: file is initialised and kept on disk.
             * Do not proceed to the read loop.                             */
            a->count = 0; a->ops_xfered = 0;
            return NULL;
        }
    }

    /* ── Phase 2: O_DIRECT random-read loop (timed) ──────────────────────
     *
     * Uses random block offsets (pread) rather than sequential reads.
     * This mirrors YCSB's random point-lookup pattern — every read lands on
     * a different LBA, defeating the SSD's sequential read-ahead cache and
     * ensuring genuine competition for read bandwidth with YCSB.           */
    int use_direct;
    int fd = open_direct(a->path, O_RDONLY, &use_direct);
    if (fd < 0) {
        fprintf(stderr, "[read %d] ERROR: open(%s) for reading: %s\n",
                a->tid, a->path, strerror(errno));
        if (!a->keep_file) unlink(a->path);
        return NULL;
    }
    if (!use_direct)
        fprintf(stderr, "[read %d] WARNING: O_DIRECT not supported; "
                "reads may be served from page cache.\n", a->tid);

    void *buf = alloc_aligned(a->io_size, 0x00);
    if (!buf) {
        fprintf(stderr, "[read %d] ERROR: posix_memalign failed\n", a->tid);
        close(fd);
        if (!a->keep_file) unlink(a->path);
        return NULL;
    }

    /* Number of full blocks in the file; each pread targets a random one. */
    uint64_t n_blocks = a->file_size / a->io_size;
    if (n_blocks == 0) n_blocks = 1;

    /* Per-thread seed so different threads access different block streams. */
    unsigned int seed = (unsigned int)a->tid * 2654435761U;

    uint64_t count = 0, bytes = 0;
    uint64_t nextNs = getNs() + a->intervalNs;
    while (g_keep_running && getNs() < a->endNs) {
        uint64_t block = (uint64_t)rand_r(&seed) % n_blocks;
        off_t off = (off_t)(block * (uint64_t)a->io_size);
        ssize_t n = pread(fd, buf, a->io_size, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[read %d] ERROR: pread at offset %ld: %s\n",
                    a->tid, (long)off, strerror(errno));
            break;
        }
        bytes += (uint64_t)n; count++;
        a->ops_xfered = count;   /* reporter reads this lock-free each second */

        if (a->intervalNs > 0) {
            uint64_t now = getNs();
            if (nextNs > now) sleepNs(nextNs - now);
            now = getNs();
            nextNs = (nextNs + a->intervalNs > now) ? nextNs + a->intervalNs : now + a->intervalNs;
        }
    }

    close(fd);
    if (!a->keep_file) unlink(a->path);
    free(buf);
    a->count = count;
    return NULL;
}

/* ── Dispatcher ───────────────────────────────────────────────────────── */

static void *worker(void *arg) {
    WorkerArgs *a = (WorkerArgs *)arg;
    return a->is_writer ? write_worker(arg) : read_worker(arg);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, const char **argv) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    if (argc < 2) {
        fprintf(stderr,
            "Usage: ./io <duration_s> [n_workers] [io_size] [dir] [max_mb]"
            " [--init-only] [--skip-init] [--rate <total_mb_s>]\n"
            "\n"
            "  duration_s   run time in seconds (init-only: ignored; runs to completion)\n"
            "  n_workers    competitor units; 1 write + 1 read thread each (default: 1)\n"
            "  io_size      bytes per op, multiple of 512 (default: 4096 = 4 KiB)\n"
            "  dir          temp file directory; use the RocksDB data disk (default: /tmp)\n"
            "  max_mb       file size per thread in MiB (default: 4096 = 4 GiB)\n"
            "  --init-only  initialise read files only, then exit (two-pass Pass 1)\n"
            "  --skip-init  skip read-file initialisation (two-pass Pass 2)\n"
            "  --rate <X>   throttle total bandwidth to X MB/s (split across workers)\n");
        return 1;
    }

    int         durationSec = atoi(argv[1]);
    uint32_t    n_workers   = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 1;
    uint32_t    io_size     = (argc >= 4) ? (uint32_t)atoi(argv[3]) : DEFAULT_IO_SIZE;
    const char *data_dir    = (argc >= 5) ? argv[4] : DEFAULT_DIR;
    uint64_t    max_mb      = (argc >= 6) ? (uint64_t)atoll(argv[5]) : DEFAULT_MAX_MB;

    /* Parse optional mode flags (may appear anywhere after positional args). */
    int init_only = 0, skip_init = 0;
    double target_rate_mbs = 0.0;
    for (int ai = 6; ai < argc; ai++) {
        if (strcmp(argv[ai], "--init-only") == 0) init_only = 1;
        else if (strcmp(argv[ai], "--skip-init") == 0) skip_init = 1;
        else if (strcmp(argv[ai], "--rate") == 0 && ai + 1 < argc) {
            target_rate_mbs = atof(argv[++ai]);
        }
        else {
            fprintf(stderr, "WARNING: unknown flag '%s' ignored\n", argv[ai]);
        }
    }
    if (init_only && skip_init) {
        fprintf(stderr, "ERROR: --init-only and --skip-init are mutually exclusive\n");
        return 1;
    }

    if (durationSec <= 0 || n_workers == 0 || io_size == 0) {
        fprintf(stderr, "ERROR: duration, n_workers, and io_size must be > 0\n");
        return 1;
    }
    if (io_size % 512 != 0) {
        fprintf(stderr,
                "ERROR: io_size (%u) must be a multiple of 512 (O_DIRECT alignment)\n",
                io_size);
        return 1;
    }
    if (max_mb == 0) {
        fprintf(stderr, "ERROR: max_mb must be > 0\n");
        return 1;
    }

    /* Round file_size down to a multiple of io_size so n_blocks is exact.  */
    uint64_t file_size = (max_mb * 1024ULL * 1024ULL / io_size) * io_size;
    if (file_size == 0) file_size = io_size;

    /* In two-pass mode, filenames omit PID so both passes use the same paths.
     * In normal mode, embed PID so concurrent invocations don't collide.   */
    int two_pass = (init_only || skip_init);
    pid_t pid    = getpid();

    uint64_t endNs = getNs() + (uint64_t)durationSec * NS_PER_S;

    /* In --init-only mode we only launch read threads (they do Phase 1).
     * Write threads are not needed and would just waste time.              */
    uint32_t numThreads = init_only ? n_workers : n_workers * 2;
    
    uint64_t intervalNs = 0;
    if (target_rate_mbs > 0.0) {
        /* Total rate is split evenly across all threads (read and write). */
        double rate_per_thread_mbs = target_rate_mbs / (double)numThreads;
        double ops_per_sec = (rate_per_thread_mbs * 1024.0 * 1024.0) / (double)io_size;
        intervalNs = (uint64_t)(NS_PER_S / ops_per_sec);
    }

    if (init_only) {
        printf("iBench IO [init-only]: initialising %u read file(s)"
               ", io_size=%u B, dir=%s, file_size=%lu MiB each\n",
               n_workers, io_size, data_dir, (unsigned long)max_mb);
    } else if (skip_init) {
        printf("iBench IO [skip-init]: %u unit(s) x %ds"
               ", io_size=%u B, dir=%s, file_size=%lu MiB/thread\n"
               "           %u write thread(s) + %u read thread(s) (random access)\n",
               n_workers, durationSec, io_size, data_dir, (unsigned long)max_mb,
               n_workers, n_workers);
    } else {
        printf("iBench IO: %u competitor unit(s) x %ds"
               ", io_size=%u B, dir=%s, file_size=%lu MiB/thread\n"
               "           %u write thread(s) + %u read thread(s) (random access)\n",
               n_workers, durationSec, io_size, data_dir, (unsigned long)max_mb,
               n_workers, n_workers);
    }

    WorkerArgs *args    = (WorkerArgs *)calloc(numThreads, sizeof(WorkerArgs));
    pthread_t  *threads = (pthread_t  *)calloc(numThreads, sizeof(pthread_t));
    if (!args || !threads) {
        fprintf(stderr, "ERROR: out of memory\n");
        return 1;
    }

    if (init_only) {
        /* Only read threads, indexed 0..n_workers-1.
         * keep_file=1: leave files on disk for Pass 2.                     */
        for (uint32_t j = 0; j < n_workers; j++) {
            args[j].tid        = (int)j;
            args[j].is_writer  = 0;
            args[j].skip_init  = 0;
            args[j].init_only  = 1;
            args[j].keep_file  = 1;  /* persist for Pass 2 */
            args[j].endNs      = endNs;
            args[j].intervalNs = 0;  /* init phase is never throttled */
            args[j].io_size    = io_size;
            args[j].file_size  = file_size;
            args[j].count      = 0;
            args[j].ops_xfered = 0;
            snprintf(args[j].path, sizeof(args[j].path),
                     "%s/iBench_io_r_%u.tmp", data_dir, j);

            if (pthread_create(&threads[j], NULL, worker, &args[j]) != 0) {
                fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", j);
                return 1;
            }
        }
    } else {
        /* Normal or skip-init mode: write threads [0..n_workers), read threads [n_workers..2n). */
        for (uint32_t i = 0; i < numThreads; i++) {
            int is_writer = (i < n_workers);
            uint32_t j    = is_writer ? i : (i - n_workers);  /* per-type index */

            args[i].tid        = (int)i;
            args[i].is_writer  = is_writer;
            args[i].skip_init  = (!is_writer && skip_init) ? 1 : 0;
            args[i].init_only  = 0;
            args[i].keep_file  = 0;  /* unlink on exit */
            args[i].endNs      = endNs;
            args[i].intervalNs = intervalNs;
            args[i].io_size    = io_size;
            args[i].file_size  = file_size;
            args[i].count      = 0;
            args[i].ops_xfered = 0;

            if (two_pass) {
                snprintf(args[i].path, sizeof(args[i].path),
                         "%s/iBench_io_%s_%u.tmp",
                         data_dir, is_writer ? "w" : "r", j);
            } else {
                snprintf(args[i].path, sizeof(args[i].path),
                         "%s/iBench_io_%s_%d_%u.tmp",
                         data_dir, is_writer ? "w" : "r", (int)pid, j);
            }

            if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
                fprintf(stderr, "ERROR: pthread_create failed for thread %u\n", i);
                return 1;
            }
        }
    }

    /* ── Reporter thread (timed runs only) ─────────────────────────────── */
    /*
     * In --init-only mode there is no timed measurement window so we skip
     * the reporter entirely.  In normal and --skip-init mode the reporter
     * wakes every second, snapshots ops_xfered across all worker threads,
     * and prints the per-second write/read/total ops/s.
     */
    ReporterArgs reporter_args;
    memset(&reporter_args, 0, sizeof(reporter_args));
    pthread_t reporter_tid;
    int reporter_launched = 0;

    if (!init_only) {
        reporter_args.workers    = args;
        reporter_args.n_workers  = n_workers;
        reporter_args.io_size    = io_size;
        reporter_args.duration_s = durationSec;
        reporter_args.stop       = 0;
        if (pthread_create(&reporter_tid, NULL, reporter_fn, &reporter_args) != 0) {
            fprintf(stderr, "WARNING: could not create reporter thread — "
                    "per-second IO stats will not be printed\n");
        } else {
            reporter_launched = 1;
        }
    }

    for (uint32_t i = 0; i < numThreads; i++)
        pthread_join(threads[i], NULL);

    if (reporter_launched) {
        reporter_args.stop = 1;
        pthread_join(reporter_tid, NULL);
    }

    /* ── Aggregate final totals ──────────────────────────────────────── */
    uint64_t total_write_ops = 0, total_read_ops = 0;
    for (uint32_t i = 0; i < numThreads; i++) {
        if (args[i].is_writer)
            total_write_ops += args[i].count;
        else
            total_read_ops  += args[i].count;
    }

    double dur       = durationSec > 0 ? (double)durationSec : 1.0;
    /* Derive MB/s from op counts for the human-readable summary. */
    double write_mb  = (double)total_write_ops * (double)io_size / (1024.0 * 1024.0);
    double read_mb   = (double)total_read_ops  * (double)io_size / (1024.0 * 1024.0);

    if (init_only) {
        printf("iBench IO [init-only]: done — %u file(s) written to %s\n",
               n_workers, data_dir);
    } else {
        printf("iBench IO: done\n");
        printf("  write: ops=%lu (%.1f ops/s), %.1f MB/s\n",
               (unsigned long)total_write_ops,
               (double)total_write_ops / dur, write_mb / dur);
        printf("  read:  ops=%lu (%.1f ops/s), %.1f MB/s\n",
               (unsigned long)total_read_ops,
               (double)total_read_ops / dur,  read_mb  / dur);
        printf("  total: %.1f ops/s  (write %.1f + read %.1f)\n",
               (double)(total_write_ops + total_read_ops) / dur,
               (double)total_write_ops / dur, (double)total_read_ops / dur);

        if (reporter_launched) {
            /*
             * Parseable summary lines for io_slack_sweep.sh summariser.
             * The Python summariser re-derives stats from per-second samples
             * with warmup_skip applied; these lines are a cross-check.
             */
            printf("io_throughput mean: %.2f stddev: %.2f min: %.2f max: %.2f"
                   "  (total ops/s, %d 1-s samples)\n",
                   reporter_args.total_mean_ops,
                   reporter_args.total_std_ops,
                   reporter_args.total_min_ops,
                   reporter_args.total_max_ops,
                   reporter_args.n_samples);
            printf("io_write_ops mean: %.2f stddev: n/a\n",
                   reporter_args.write_mean_ops);
            printf("io_read_ops mean: %.2f stddev: n/a\n",
                   reporter_args.read_mean_ops);
            fflush(stdout);
        }
    }

    free(args);
    free(threads);
    return 0;
}
