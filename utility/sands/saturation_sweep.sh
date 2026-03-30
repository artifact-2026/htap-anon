#!/usr/bin/env bash
# =============================================================================
# saturation_sweep.sh — RocksDB single-workload characterization sweep
#
# Sweeps client thread counts for one YCSB workload and records per-thread
# throughput, disk I/O, CPU, and memory utilization.  All results are
# consolidated into a single summary CSV that the standalone plotting script
# (plot_bottleneck.py) can consume alongside CSVs from other nodes/workloads.
#
# Run from the project BUILD directory:
#   cd /path/to/build && bash ../utility/saturation_sweep.sh
#
# ── Selecting the workload ────────────────────────────────────────────────────
# Required: set WORKLOAD_SPEC to the path of a YCSB .spec file.
# Optional: set WORKLOAD_LABEL to a human-readable name used in the CSV and
#           in plot titles (defaults to the spec filename without extension).
#
#   WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#   WORKLOAD_LABEL="IO-bound (workload A)" \
#   bash ../utility/saturation_sweep.sh \
#     --disk-read-max-bandwidth=3500 \
#     --disk-write-max-bandwidth=3000 \
#     --iops-max=500000
#
# The spec file controls the operation mix (readproportion, updateproportion,
# scanproportion, insertproportion).  Infra overrides (fieldcount, fieldlength,
# recordcount, metrics_output) are appended automatically so they always win.
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   load.log                   — DB load phase output
#   sweep/
#     run_t<N>/
#       ycsb_t<N>.log          — raw YCSB output for this thread count
#       system_t<N>.csv        — per-second CPU / disk / memory samples
#       compaction_metrics.csv — per-compaction-event RocksDB metrics
#   summary.csv                — THE deliverable: one row per thread count,
#                                all metrics aggregated over the steady-state
#                                window.  Feed this to plot_bottleneck.py.
#
# ── Key parsing note ──────────────────────────────────────────────────────────
# YCSB prints:  throughput mean:<mean>  stddev: <stddev>
# after a hard-coded 60 s warmup skip inside runXput().
# The system monitor collects per-second CPU/disk/mem; skip the same 60 s here.
#
# ── Configuration ─────────────────────────────────────────────────────────────
# =============================================================================

set -euo pipefail

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./sat_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── Workload ──────────────────────────────────────────────────────────────────
# WORKLOAD_SPEC: path to a YCSB .spec file (required — no default).
# WORKLOAD_LABEL: string written into summary.csv and used by the plotter.
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"
WORKLOAD_LABEL="${WORKLOAD_LABEL:-}"

# ── Experiment knobs ──────────────────────────────────────────────────────────

# Thread counts to sweep.
THREAD_COUNTS="${THREAD_COUNTS:-1 2 4 8 12 16 20 24 32 48}"

# Total experiment duration per thread count (seconds).
# YCSB skips the first WARMUP_SKIP_S seconds; the rest is the steady window.
# Keep RUNTIME_SECS ≥ 90 s for meaningful stddev.  120 s → 60 s steady window.
RUNTIME_SECS="${RUNTIME_SECS:-120}"

# Block device for disk I/O monitoring.  Find yours with: lsblk / df -h <dbpath>
# Use the block device name as it appears in /proc/diskstats (e.g. nvme0n1, sda).
# NOTE: nvme0n1 is the block device; nvme0c0n1 is the NVMe character device and
#       will NOT appear in /proc/diskstats — do not use the c0 form here.
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Peak sequential read bandwidth of the device in MB/s (measured with fio).
# Used in disk_bandwidth_pct = read_mb/s/DISK_READ_MAX_BANDWIDTH + write_mb/s/DISK_WRITE_MAX_BANDWIDTH.
# Set to 0 to leave disk_bandwidth_pct as NaN.
DISK_READ_MAX_BANDWIDTH="${DISK_READ_MAX_BANDWIDTH:-0}"

# Peak sequential write bandwidth of the device in MB/s (measured with fio).
DISK_WRITE_MAX_BANDWIDTH="${DISK_WRITE_MAX_BANDWIDTH:-0}"

# Peak IOPS of the device (measured with fio, random 4K mix).
# Used in iops_pct = r/s / IOPS_READ_MAX + w/s / IOPS_WRITE_MAX.  Set to 0 to leave iops_pct as NaN.
IOPS_READ_MAX="${IOPS_READ_MAX:-0}"
IOPS_WRITE_MAX="${IOPS_WRITE_MAX:-0}"

# Warmup seconds to skip when computing CPU/disk/memory stats from the system CSV.
# Must match YCSB's internal skip (hardcoded 60 s in runXput).
WARMUP_SKIP_S="${WARMUP_SKIP_S:-60}"

# Drop the OS page cache before every run so that disk reads are not silently
# served from RAM.  Requires passwordless sudo for tee /proc/sys/vm/drop_caches,
# or run the script as root.  Set to false to skip (reads may show as 0 MB/s).
DROP_CACHES="${DROP_CACHES:-true}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
SUMMARIZER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""

SAT_TMPDIR="/holly/htap/build"

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]]       && kill "$MONITOR_PID" 2>/dev/null || true
    [[ -n "${MONITOR_SCRIPT:-}" ]]    && rm -f "$MONITOR_SCRIPT"
    [[ -n "${SUMMARIZER_SCRIPT:-}" ]] && rm -f "$SUMMARIZER_SCRIPT"
    [[ -n "${TMPSPEC:-}" ]]           && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]] || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC is not set.  Point it at a .spec file."
    [[ -f "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC file not found: $WORKLOAD_SPEC"
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    python3 -c "import pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user pandas numpy -q \
            || sudo apt-get install -y python3-pandas python3-numpy -q
    }
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: device '$DISK_DEVICE' not found in /proc/diskstats — disk I/O will be zero."
        log "  Available: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
    fi
    (( RUNTIME_SECS > WARMUP_SKIP_S )) || \
        die "RUNTIME_SECS ($RUNTIME_SECS) must be > WARMUP_SKIP_S ($WARMUP_SKIP_S)"

    # Derive label from spec filename if not provided.
    if [[ -z "$WORKLOAD_LABEL" ]]; then
        WORKLOAD_LABEL="$(basename "$WORKLOAD_SPEC" .spec)"
    fi
}

# ── Embedded per-second system monitor ───────────────────────────────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp "${SAT_TMPDIR}/sat_monitor_XXXXX.py")
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
"""Per-second CPU, disk I/O, and memory monitor. Writes a flushed CSV."""
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]  # user nice sys idle iowait irq softirq steal

def read_disk(dev):
    if not dev:
        return 0, 0, 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                # sectors_read, sectors_written, reads_completed, writes_completed
                return int(p[5]), int(p[9]), int(p[3]), int(p[7])
    return 0, 0, 0, 0

def read_mem_mib():
    info = {}
    with open('/proc/meminfo') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                info[parts[0].rstrip(':')] = int(parts[1])  # kB
    total = info.get('MemTotal', 0) / 1024
    avail = info.get('MemAvailable', 0) / 1024
    return total, avail, total - avail  # total, avail, used (MiB)

prev_cpu                        = read_cpu()
prev_rd, prev_wr, prev_rc, prev_wc = read_disk(disk_device)
prev_t                          = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s',
                'cpu_user_pct', 'cpu_sys_pct', 'cpu_iowait_pct', 'cpu_idle_pct',
                'disk_read_mbs', 'disk_write_mbs',
                'disk_read_iops', 'disk_write_iops',
                'mem_total_mib', 'mem_used_mib', 'mem_avail_mib'])
    fh.flush()
    while True:
        time.sleep(1)
        now                             = time.time()
        cur_cpu                         = read_cpu()
        cur_rd, cur_wr, cur_rc, cur_wc  = read_disk(disk_device)
        m_total, m_avail, m_used        = read_mem_mib()

        dt    = (now - prev_t) or 1
        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        w.writerow([int(now),
                    f'{100*delta[0]/total:.2f}',              # user
                    f'{100*delta[2]/total:.2f}',              # sys
                    f'{100*delta[4]/total:.2f}',              # iowait
                    f'{100*delta[3]/total:.2f}',              # idle
                    f'{(cur_rd-prev_rd)*512/1024/1024/dt:.3f}',  # read MB/s
                    f'{(cur_wr-prev_wr)*512/1024/1024/dt:.3f}',  # write MB/s
                    f'{(cur_rc-prev_rc)/dt:.2f}',             # r/s
                    f'{(cur_wc-prev_wc)/dt:.2f}',             # w/s
                    f'{m_total:.1f}', f'{m_used:.1f}', f'{m_avail:.1f}'])
        fh.flush()
        prev_cpu = cur_cpu
        prev_rd, prev_wr, prev_rc, prev_wc = cur_rd, cur_wr, cur_rc, cur_wc
        prev_t = now
PYMON
}

start_monitor() { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor() {
    [[ -n "${MONITOR_PID:-}" ]] && {
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
        MONITOR_PID=""
    }
}

# ── Page-cache flush ──────────────────────────────────────────────────────────
# Without this, the kernel serves RocksDB reads from the OS page cache (populated
# during the load phase) so /proc/diskstats never sees any read I/O.
# sync first to flush dirty pages so drop_caches doesn't lose data.

drop_page_cache() {
    if [[ "$DROP_CACHES" != "true" ]]; then
        return
    fi
    log "  Dropping OS page cache (sync + drop_caches=3) ..."
    # Flush dirty pages via sysctl rather than the sync binary, which may be
    # built for a different architecture in some environments.
    sudo sysctl -w vm.dirty_expire_centisecs=0 > /dev/null 2>&1 || true
    if echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
        log "  Page cache dropped."
    else
        log "  WARNING: drop_caches failed (need sudo or root). Disk reads may still be 0."
        log "           Re-run as root or grant passwordless sudo for tee /proc/sys/vm/drop_caches."
    fi
}

# ── Spec builder ──────────────────────────────────────────────────────────────
# Always makes a fresh temp copy of WORKLOAD_SPEC so that load_db's
# "rm -f $spec" never deletes the original file.  fieldcount, fieldlength,
# recordcount, and operationcount come entirely from the spec itself.
# Callers may append extra key=value pairs as positional arguments
# (e.g. create_spec "metrics_output=/path/to/file").

create_spec() {
    TMPSPEC=$(mktemp "${SAT_TMPDIR}/sat_spec_XXXXX.spec")
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────

load_db() {
    local dbpath="$1"
    local rc; rc=$(grep -E '^recordcount\s*=' "$WORKLOAD_SPEC" | tail -1 | cut -d= -f2 | tr -d ' ')
    log "Loading ${rc:-unknown} records into $dbpath ..."
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap true -threads 8 \
        -load true -run false -throughput false \
        -runtime 0 \
        -levels 7 -table baseline \
        2>&1 | tee "$OUTPUT_DIR/load.log"
    rm -f "$spec"; TMPSPEC=""
    log "Load complete."
}

# ── Single thread-count run ───────────────────────────────────────────────────

run_one() {
    local threads="$1" run_dir="$2" dbpath="$3"

    local sys_csv="$run_dir/system_t${threads}.csv"
    local log_file="$run_dir/ycsb_t${threads}.log"
    local cmp_csv="$run_dir/compaction_metrics.csv"

    local spec; spec=$(create_spec "metrics_output=${cmp_csv}")

    drop_page_cache
    log "  Running $threads thread(s) for ${RUNTIME_SECS}s ..."
    start_monitor "$sys_csv"
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$threads" \
        -load false -run false -throughput true \
        -runtime "$RUNTIME_SECS" \
        -levels 7 -table baseline \
        -dbstatistics true \
        2>&1 | tee "$log_file"
    stop_monitor

    rm -f "$spec"; TMPSPEC=""
    log "  → log:        $log_file"
    log "  → sys:        $sys_csv"
    log "  → compaction: $cmp_csv"
}

# ── CSV summarizer ────────────────────────────────────────────────────────────
# Reads all per-thread-count raw files and writes OUTPUT_DIR/summary.csv.
# One row per thread count; columns are the metrics needed by plot_bottleneck.py.

setup_summarizer_script() {
    SUMMARIZER_SCRIPT=$(mktemp "${SAT_TMPDIR}/sat_summarize_XXXXX.py")
    cat > "$SUMMARIZER_SCRIPT" << 'PYSUM'
#!/usr/bin/env python3
"""
Aggregate per-thread raw files → summary.csv

Columns written
---------------
workload_label          : human label (from argv)
threads                 : client thread count
xput_mean               : mean throughput (ops/s) over steady-state window
xput_std                : std-dev of per-second throughput
cpu_active_mean         : mean CPU utilization % (100 - idle)
cpu_active_std
disk_read_mb/s          : mean disk read bandwidth (MB/s) over steady-state window
disk_read_std
disk_write_mb/s         : mean disk write bandwidth (MB/s) over steady-state window
disk_write_std
r/s                     : mean disk read IOPS over steady-state window
r/s_std
w/s                     : mean disk write IOPS over steady-state window
w/s_std
disk_bandwidth_pct      : disk_read_mb/s / DISK_READ_MAX_BANDWIDTH
                          + disk_write_mb/s / DISK_WRITE_MAX_BANDWIDTH
                          (NaN if either ceiling is 0)
iops_pct                : r/s / IOPS_READ_MAX + w/s / IOPS_WRITE_MAX  (NaN if eith IOPS_MAX is 0)
mem_used_mean           : mean memory used (GiB)  — system-wide, from /proc/meminfo
mem_used_std
mem_avail_mean          : mean memory available (GiB)
mem_used_pct_mean       : mem_used / mem_total * 100  (supplementary context)
block_cache_hits        : cumulative block cache hits over the full run
block_cache_misses      : cumulative block cache misses over the full run
block_cache_hit_rate    : hits / (hits + misses)  — primary memory-bound indicator

Note on block_cache_hit_rate vs mem_used_pct
--------------------------------------------
mem_used_pct shows gross RAM pressure but cannot distinguish between a workload
that is IO-bound (working set exceeds cache, lots of disk reads) and one that is
memory-bound (working set fits in cache, reads served from DRAM).
block_cache_hit_rate is the direct indicator: a high rate (≈ 1.0) combined with
low disk I/O and a throughput plateau identifies a memory-bound workload.
These are cumulative over the full run (including warmup) since RocksDB
statistics are printed once at the end via PrintStats().
"""
import sys, os, re, glob
import pandas as pd
import numpy as np

sweep_dir              = sys.argv[1]
output_csv             = sys.argv[2]
warmup_skip            = int(sys.argv[3])
workload_label         = sys.argv[4]
disk_read_max_bw       = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0
disk_write_max_bw      = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0
iops_read_max          = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0
iops_write_max         = float(sys.argv[8]) if len(sys.argv) > 8 else 0.0

# ── helpers ──────────────────────────────────────────────────────────────────

def thread_from_path(p):
    m = re.search(r'_t(\d+)', os.path.basename(p))
    return int(m.group(1)) if m else 0

def parse_ycsb_log(path):
    """Return (xput_mean, xput_std, cache_hits, cache_misses)."""
    nan = float('nan')
    xput_mean = xput_std = nan
    cache_hits = cache_misses = nan
    try:
        text = open(path).read()

        # Throughput summary line written by runXput().
        m = re.search(
            r'throughput mean:\s*([\d.eE+\-]+)\s+stddev:\s*([\d.eE+\-]+)', text)
        if m:
            xput_mean, xput_std = float(m.group(1)), float(m.group(2))

        # RocksDB Statistics block written by PrintStats() when -dbstatistics true.
        # Format (from Statistics::ToString):
        #   rocksdb.block.cache.hit COUNT : 1234567
        #   rocksdb.block.cache.miss COUNT : 12345
        mh = re.search(r'rocksdb\.block\.cache\.hit\s+COUNT\s*:\s*(\d+)', text)
        mm = re.search(r'rocksdb\.block\.cache\.miss\s+COUNT\s*:\s*(\d+)', text)
        if mh:
            cache_hits = int(mh.group(1))
        if mm:
            cache_misses = int(mm.group(1))

    except Exception as e:
        print(f'  [warn] {path}: {e}')
    return xput_mean, xput_std, cache_hits, cache_misses

def parse_system_csv(path, skip_s):
    nan = float('nan')
    empty = dict(
        cpu_active_mean=nan,  cpu_active_std=nan,
        disk_read_mean=nan,   disk_read_std=nan,
        disk_write_mean=nan,  disk_write_std=nan,
        disk_read_iops_mean=nan,  disk_read_iops_std=nan,
        disk_write_iops_mean=nan, disk_write_iops_std=nan,
        mem_used_mean=nan,    mem_used_std=nan,
        mem_avail_mean=nan,   mem_used_pct_mean=nan,
    )
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        steady = df[df['elapsed_s'] >= skip_s]
        if steady.empty:
            steady = df
        active = 100.0 - steady['cpu_idle_pct']
        dr = steady['disk_read_mbs']
        dw = steady['disk_write_mbs']
        # IOPS columns (present in new-format CSVs; fall back to NaN if absent)
        dri = steady['disk_read_iops']  if 'disk_read_iops'  in steady.columns \
              else pd.Series(dtype=float)
        dwi = steady['disk_write_iops'] if 'disk_write_iops' in steady.columns \
              else pd.Series(dtype=float)
        has_mem = 'mem_used_mib' in steady.columns
        if has_mem:
            mu  = steady['mem_used_mib']  / 1024   # GiB
            ma  = steady['mem_avail_mib'] / 1024   # GiB
            mt  = steady['mem_total_mib'] / 1024   # GiB
            pct = (steady['mem_used_mib'] / steady['mem_total_mib'] * 100)
        else:
            mu = ma = mt = pct = pd.Series(dtype=float)
        return dict(
            cpu_active_mean      = active.mean(),
            cpu_active_std       = active.std(ddof=1),
            disk_read_mean       = dr.mean(),
            disk_read_std        = dr.std(ddof=1),
            disk_write_mean      = dw.mean(),
            disk_write_std       = dw.std(ddof=1),
            disk_read_iops_mean  = dri.mean()      if not dri.empty else nan,
            disk_read_iops_std   = dri.std(ddof=1) if not dri.empty else nan,
            disk_write_iops_mean = dwi.mean()      if not dwi.empty else nan,
            disk_write_iops_std  = dwi.std(ddof=1) if not dwi.empty else nan,
            mem_used_mean        = mu.mean()       if not mu.empty  else nan,
            mem_used_std         = mu.std(ddof=1)  if not mu.empty  else nan,
            mem_avail_mean       = ma.mean()       if not ma.empty  else nan,
            mem_used_pct_mean    = pct.mean()      if not pct.empty else nan,
        )
    except Exception as e:
        print(f'  [warn] {path}: {e}')
        return empty

# ── collect ───────────────────────────────────────────────────────────────────

run_dirs = sorted(
    glob.glob(os.path.join(sweep_dir, 'run_t*')),
    key=thread_from_path)

rows = []
for rd in run_dirs:
    t = thread_from_path(rd)
    xput_mean, xput_std, cache_hits, cache_misses = \
        parse_ycsb_log(os.path.join(rd, f'ycsb_t{t}.log'))
    sys_stats = parse_system_csv(os.path.join(rd, f'system_t{t}.csv'), warmup_skip)

    disk_r  = sys_stats['disk_read_mean']
    disk_w  = sys_stats['disk_write_mean']
    disk_rs = sys_stats['disk_read_std']
    disk_ws = sys_stats['disk_write_std']
    iops_r  = sys_stats['disk_read_iops_mean']
    iops_w  = sys_stats['disk_write_iops_mean']
    iops_rs = sys_stats['disk_read_iops_std']
    iops_ws = sys_stats['disk_write_iops_std']

    # disk_bandwidth_pct = read_mb/s / read_max + write_mb/s / write_max
    # (each term is independently normalised, so the sum reflects mixed pressure)
    if disk_read_max_bw > 0 and disk_write_max_bw > 0:
        r_term = (disk_r if not np.isnan(disk_r) else 0) / disk_read_max_bw
        w_term = (disk_w if not np.isnan(disk_w) else 0) / disk_write_max_bw
        disk_bandwidth_pct = r_term + w_term
    else:
        disk_bandwidth_pct = float('nan')

    # iops_pct = r/s / IOPS_READ_MAX + w/s / IOPS_WRITE_MAX
    if iops_read_max > 0 and iops_write_max > 0:
        iops_pct = (iops_r if not np.isnan(iops_r) else 0) / iops_read_max \
                   + (iops_w if not np.isnan(iops_w) else 0) / iops_write_max
    else:
        iops_pct = float('nan')

    # Block cache hit rate — nan if stats were not printed.
    if not (np.isnan(cache_hits) or np.isnan(cache_misses)):
        total_accesses = cache_hits + cache_misses
        hit_rate = cache_hits / total_accesses if total_accesses > 0 else float('nan')
    else:
        hit_rate = float('nan')

    rows.append(dict(
        workload_label        = workload_label,
        threads               = t,
        xput_mean             = xput_mean,
        xput_std              = xput_std,
        cpu_active_mean       = sys_stats['cpu_active_mean'],
        cpu_active_std        = sys_stats['cpu_active_std'],
        **{'disk_read_mb/s'   : disk_r},
        disk_read_std         = disk_rs,
        **{'disk_write_mb/s'  : disk_w},
        disk_write_std        = disk_ws,
        **{'r/s'              : iops_r},
        **{'r/s_std'          : iops_rs},
        **{'w/s'              : iops_w},
        **{'w/s_std'          : iops_ws},
        disk_bandwidth_pct    = disk_bandwidth_pct * 100,
        iops_pct              = iops_pct * 100,
        mem_used_mean         = sys_stats['mem_used_mean'],
        mem_used_std          = sys_stats['mem_used_std'],
        mem_avail_mean        = sys_stats['mem_avail_mean'],
        mem_used_pct_mean     = sys_stats['mem_used_pct_mean'],
        block_cache_hits      = cache_hits,
        block_cache_misses    = cache_misses,
        block_cache_hit_rate  = hit_rate,
    ))

if not rows:
    print('No run directories found — summary.csv not written.')
    sys.exit(0)

df = pd.DataFrame(rows).sort_values('threads').reset_index(drop=True)
df.to_csv(output_csv, index=False)
print(f'summary.csv written → {output_csv}')
preview_cols = ['threads', 'xput_mean', 'cpu_active_mean',
                'disk_read_mb/s', 'disk_write_mb/s', 'r/s', 'w/s',
                'disk_bandwidth_pct', 'iops_pct',
                'mem_used_pct_mean', 'block_cache_hit_rate']
print(df[[c for c in preview_cols if c in df.columns]].to_string(index=False))
PYSUM
}

run_summarizer() {
    sep
    log "Aggregating results → summary.csv ..."
    setup_summarizer_script
    python3 "$SUMMARIZER_SCRIPT" \
        "$OUTPUT_DIR/sweep" \
        "$OUTPUT_DIR/summary.csv" \
        "$WARMUP_SKIP_S" \
        "$WORKLOAD_LABEL" \
        "$DISK_READ_MAX_BANDWIDTH" \
        "$DISK_WRITE_MAX_BANDWIDTH" \
        "$IOPS_READ_MAX" \
        "$IOPS_WRITE_MAX"
    log "  → $OUTPUT_DIR/summary.csv"
}

# ── Main sweep ────────────────────────────────────────────────────────────────

run_sweep() {
    sep
    log "Sweep: $WORKLOAD_LABEL"
    log "  Spec:    $WORKLOAD_SPEC"
    log "  Threads: $THREAD_COUNTS"
    log "  Runtime: ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"
    local dbpath="$sweep_dir/rocksdb"

    recordcount=$(awk -F= '$1=="recordcount" {print $2}' "$WORKLOAD_SPEC")

    load_db "$dbpath"

    for threads in $THREAD_COUNTS; do
        sep
        log "Thread count: $threads"
        local run_dir="$sweep_dir/run_t${threads}"
        mkdir -p "$run_dir"
        run_one "$threads" "$run_dir" "$dbpath"
    done

    log "Sweep complete."
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    # ── Command-line flags (override env vars) ────────────────────────────────
    # Accepted forms: --flag=VALUE  or  --flag VALUE
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --disk-read-max-bandwidth=*)  DISK_READ_MAX_BANDWIDTH="${1#*=}" ;;
            --disk-read-max-bandwidth)    DISK_READ_MAX_BANDWIDTH="$2"; shift ;;
            --disk-write-max-bandwidth=*) DISK_WRITE_MAX_BANDWIDTH="${1#*=}" ;;
            --disk-write-max-bandwidth)   DISK_WRITE_MAX_BANDWIDTH="$2"; shift ;;
            --iops-read-max=*)            IOPS_READ_MAX="${1#*=}" ;;
            --iops-read-max)              IOPS_READ_MAX="$2"; shift ;;
            --iops-write-max=*)           IOPS_WRITE_MAX="${1#*=}" ;;
            --iops-write-max)             IOPS_WRITE_MAX="$2"; shift ;;
            *) die "Unknown argument: $1.  Use env vars or --disk-read-max-bandwidth, --disk-write-max-bandwidth, --iops-read-max, --iops-write-max" ;;
        esac
        shift
    done

    check_prereqs

    log "saturation_sweep.sh starting"
    log "  Binary:              $BINARY"
    log "  Output dir:          $OUTPUT_DIR"
    log "  Device:              $DISK_DEVICE"
    log "  Workload:            $WORKLOAD_LABEL"
    log "  Spec:                $WORKLOAD_SPEC"
    log "  Threads:             $THREAD_COUNTS"
    log "  Runtime/pt:          ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    log "  Disk read max BW:    ${DISK_READ_MAX_BANDWIDTH} MB/s"
    log "  Disk write max BW:   ${DISK_WRITE_MAX_BANDWIDTH} MB/s"
    log "  IOPS read max:       ${IOPS_READ_MAX}"
    log "  IOPS write max:      ${IOPS_WRITE_MAX}"
    echo ""

    python3 -c "import pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user pandas numpy -q
    }

    mkdir -p "$OUTPUT_DIR"
    setup_monitor_script

    run_sweep
    run_summarizer

    sep
    log "Done."
    log "  Results:     $OUTPUT_DIR"
    log "  Summary CSV: $OUTPUT_DIR/summary.csv"
    log ""
    log "  When you have all workload CSVs, plot with:"
    log "    python3 plot_bottleneck.py \\"
    log "      --csv io-bound:node1/summary.csv \\"
    log "      --csv cpu-bound:node2/summary.csv \\"
    log "      --csv mem-bound:node3/summary.csv \\"
    log "      --csv none-bound:node4/summary.csv \\"
    log "      --out bottleneck_characterization.pdf"
}

main "$@"
