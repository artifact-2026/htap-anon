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
#   bash ../utility/saturation_sweep.sh
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
DISK_DEVICE="${DISK_DEVICE:-nvme0c0n1}"

# Warmup seconds to skip when computing CPU/disk/memory stats from the system CSV.
# Must match YCSB's internal skip (hardcoded 60 s in runXput).
WARMUP_SKIP_S="${WARMUP_SKIP_S:-60}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
SUMMARIZER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""

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
    MONITOR_SCRIPT=$(mktemp /tmp/sat_monitor_XXXXX.py)
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
        return 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                return int(p[5]), int(p[9])  # sectors_read, sectors_written
    return 0, 0

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

prev_cpu          = read_cpu()
prev_rd, prev_wr  = read_disk(disk_device)
prev_t            = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s',
                'cpu_user_pct', 'cpu_sys_pct', 'cpu_iowait_pct', 'cpu_idle_pct',
                'disk_read_mbs', 'disk_write_mbs',
                'mem_total_mib', 'mem_used_mib', 'mem_avail_mib'])
    fh.flush()
    while True:
        time.sleep(1)
        now            = time.time()
        cur_cpu        = read_cpu()
        cur_rd, cur_wr = read_disk(disk_device)
        m_total, m_avail, m_used = read_mem_mib()

        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        w.writerow([int(now),
                    f'{100*delta[0]/total:.2f}',  # user
                    f'{100*delta[2]/total:.2f}',  # sys
                    f'{100*delta[4]/total:.2f}',  # iowait
                    f'{100*delta[3]/total:.2f}',  # idle
                    f'{(cur_rd-prev_rd)*512/1024/1024/((now-prev_t) or 1):.3f}',
                    f'{(cur_wr-prev_wr)*512/1024/1024/((now-prev_t) or 1):.3f}',
                    f'{m_total:.1f}', f'{m_used:.1f}', f'{m_avail:.1f}'])
        fh.flush()
        prev_cpu = cur_cpu; prev_rd, prev_wr = cur_rd, cur_wr; prev_t = now
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

# ── Spec builder ──────────────────────────────────────────────────────────────
# Copies the user's workload spec and appends infra overrides so they win over
# anything the spec file might set for fieldcount, fieldlength, etc.

create_spec() {
    # If no extra key=value pairs are passed, just use WORKLOAD_SPEC directly.
    if [ "$#" -eq 0 ]; then
        echo "$WORKLOAD_SPEC"
        return
    fi

    # Otherwise, make a temp copy of WORKLOAD_SPEC and append only the extras.
    TMPSPEC=$(mktemp /tmp/sat_spec_XXXXX.spec)
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────

load_db() {
    local dbpath="$1"
    log "Loading $recordcount records into $dbpath ..."
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
    SUMMARIZER_SCRIPT=$(mktemp /tmp/sat_summarize_XXXXX.py)
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
disk_read_mean          : mean disk read bandwidth (MB/s)
disk_read_std
disk_write_mean         : mean disk write bandwidth (MB/s)
disk_write_std
disk_total_mean         : disk_read_mean + disk_write_mean
disk_total_std          : sqrt(read_std^2 + write_std^2)
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

sweep_dir      = sys.argv[1]
output_csv     = sys.argv[2]
warmup_skip    = int(sys.argv[3])
workload_label = sys.argv[4]

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
        cpu_active_mean=nan, cpu_active_std=nan,
        disk_read_mean=nan,  disk_read_std=nan,
        disk_write_mean=nan, disk_write_std=nan,
        mem_used_mean=nan,   mem_used_std=nan,
        mem_avail_mean=nan,  mem_used_pct_mean=nan,
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
        has_mem = 'mem_used_mib' in steady.columns
        if has_mem:
            mu = steady['mem_used_mib']  / 1024   # GiB
            ma = steady['mem_avail_mib'] / 1024   # GiB
            mt = steady['mem_total_mib'] / 1024   # GiB
            pct = (steady['mem_used_mib'] / steady['mem_total_mib'] * 100)
        else:
            mu = ma = mt = pct = pd.Series(dtype=float)
        return dict(
            cpu_active_mean  = active.mean(),
            cpu_active_std   = active.std(ddof=1),
            disk_read_mean   = dr.mean(),
            disk_read_std    = dr.std(ddof=1),
            disk_write_mean  = dw.mean(),
            disk_write_std   = dw.std(ddof=1),
            mem_used_mean    = mu.mean()      if not mu.empty else nan,
            mem_used_std     = mu.std(ddof=1) if not mu.empty else nan,
            mem_avail_mean   = ma.mean()      if not ma.empty else nan,
            mem_used_pct_mean= pct.mean()     if not pct.empty else nan,
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

    # Block cache hit rate — nan if stats were not printed.
    if not (np.isnan(cache_hits) or np.isnan(cache_misses)):
        total_accesses = cache_hits + cache_misses
        hit_rate = cache_hits / total_accesses if total_accesses > 0 else float('nan')
    else:
        hit_rate = float('nan')

    rows.append(dict(
        workload_label       = workload_label,
        threads              = t,
        xput_mean            = xput_mean,
        xput_std             = xput_std,
        cpu_active_mean      = sys_stats['cpu_active_mean'],
        cpu_active_std       = sys_stats['cpu_active_std'],
        disk_read_mean       = disk_r,
        disk_read_std        = disk_rs,
        disk_write_mean      = disk_w,
        disk_write_std       = disk_ws,
        disk_total_mean      = (disk_r  if not np.isnan(disk_r)  else 0)
                             + (disk_w  if not np.isnan(disk_w)  else 0),
        disk_total_std       = np.sqrt(
                               (disk_rs**2 if not np.isnan(disk_rs) else 0) +
                               (disk_ws**2 if not np.isnan(disk_ws) else 0)),
        mem_used_mean        = sys_stats['mem_used_mean'],
        mem_used_std         = sys_stats['mem_used_std'],
        mem_avail_mean       = sys_stats['mem_avail_mean'],
        mem_used_pct_mean    = sys_stats['mem_used_pct_mean'],
        block_cache_hits     = cache_hits,
        block_cache_misses   = cache_misses,
        block_cache_hit_rate = hit_rate,
    ))

if not rows:
    print('No run directories found — summary.csv not written.')
    sys.exit(0)

df = pd.DataFrame(rows).sort_values('threads').reset_index(drop=True)
df.to_csv(output_csv, index=False)
print(f'summary.csv written → {output_csv}')
print(df[['threads', 'xput_mean', 'cpu_active_mean', 'disk_total_mean',
          'mem_used_pct_mean', 'block_cache_hit_rate']].to_string(index=False))
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
        "$WORKLOAD_LABEL"
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
    check_prereqs

    log "saturation_sweep.sh starting"
    log "  Binary:       $BINARY"
    log "  Output dir:   $OUTPUT_DIR"
    log "  Device:       $DISK_DEVICE"
    log "  Workload:     $WORKLOAD_LABEL"
    log "  Spec:         $WORKLOAD_SPEC"
    log "  Threads:      $THREAD_COUNTS"
    log "  Runtime/pt:   ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
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
