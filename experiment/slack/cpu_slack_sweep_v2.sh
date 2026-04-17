#!/usr/bin/env bash
# =============================================================================
# cpu_slack_sweep_v2.sh — CPU slack characterization (fresh-load per point)
#
# Mirrors saturation_sweep.sh exactly, except the sweep variable is the number
# of concurrent CPU (iBench) workers rather than YCSB thread count.
# KNEE_THREADS is fixed throughout; for each worker count we:
#   1. Wipe and reload a fresh RocksDB (same load path as saturation_sweep.sh)
#   2. Drop the OS page cache
#   3. Start YCSB in the background at KNEE_THREADS threads for RUNTIME_SECS
#   4. Immediately start N_WORKERS iBench_cpu workers (also for RUNTIME_SECS)
#   5. Wait for YCSB to finish → parse xput mean/std/min/max
#   6. Wait/kill workers, stop monitor
#
# This gives one statistically independent data point per worker count with no
# compaction-state confounding between points.
#
# Run from the project BUILD directory:
#   cd /path/to/build
#   KNEE_THREADS=16 WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#     bash ../utility/cpu_slack_sweep_v2.sh
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   sweep/
#     run_w<N>/
#       load.log                 — DB load phase
#       ycsb_w<N>.log            — raw YCSB output
#       system_w<N>.csv          — per-second CPU/disk/mem samples
#       compaction_metrics.csv   — per-compaction-event RocksDB metrics
#   summary.csv                  — one row per worker count; same schema as
#                                  saturation_sweep.sh plus a 'workers' column
# =============================================================================

set -euo pipefail

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./slack_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── Workload ──────────────────────────────────────────────────────────────────
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"
WORKLOAD_LABEL="${WORKLOAD_LABEL:-}"

# ── Experiment knobs ──────────────────────────────────────────────────────────

# Fixed YCSB thread count (the saturation knee).
KNEE_THREADS="${KNEE_THREADS:-16}"

# CPU worker counts to sweep (space-separated).
WORKER_COUNTS="${WORKER_COUNTS:-0 1 2 4 8 16 32}"

# Number of independent trials per worker count (median is reported).
TRIALS_PER_POINT="${TRIALS_PER_POINT:-1}"

# Total experiment duration per point in seconds (= full YCSB runtime).
# With WARMUP_SKIP_S=0 the entire 15-minute window contributes to the reported mean.
RUNTIME_SECS="${RUNTIME_SECS:-900}"

# Warmup seconds to discard from per-second system CSV when summarising.
# Set to 0 to report over the full RUNTIME_SECS window.
WARMUP_SKIP_S="${WARMUP_SKIP_S:-0}"

# Records to preload (default 5 M from spec; override here to take precedence).
# Leave blank to use whatever recordcount is in WORKLOAD_SPEC.
RECORD_COUNT="${RECORD_COUNT:-10000000}"

# Block device to monitor (e.g. nvme0n1). Auto-detected if empty.
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Drop OS page cache before every run (requires passwordless sudo).
DROP_CACHES="${DROP_CACHES:-true}"

# ── iBench_cpu worker knobs ───────────────────────────────────────────────────

# Per-worker transform rate (0 = busy-loop / saturate one core).
TRANSFORMS_PER_WORKER="${TRANSFORMS_PER_WORKER:-0}"

# Arithmetic rounds per transform.
WORKER_ROUNDS_MULTIPLIER="${WORKER_ROUNDS_MULTIPLIER:-8}"

# Optional CPU affinity base for iBench_cpu workers.
# When set, worker i is pinned to CPU (CPU_AFFINITY_BASE + i).
# Prevents Linux CFS "sleeper fairness" from boosting YCSB threads above the
# busy-loop iBench workers and causing throughput to *increase* with workers.
# Leave unset (default) to let the OS schedule freely.
#   Example: CPU_AFFINITY_BASE=32  # pin workers to cores 32, 33, 34, ...
CPU_AFFINITY_BASE="${CPU_AFFINITY_BASE:-}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
SUMMARIZER_SCRIPT=""
TRANSFORM_BINARY=""
TMPSPEC=""
MONITOR_PID=""
declare -a TRANSFORM_PIDS=()

SAT_TMPDIR="/holly/htap/build"

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]]     && kill "$MONITOR_PID"   2>/dev/null || true
    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    [[ -n "${MONITOR_SCRIPT:-}" ]]   && rm -f "$MONITOR_SCRIPT"
    [[ -n "${SUMMARIZER_SCRIPT:-}" ]] && rm -f "$SUMMARIZER_SCRIPT"
    [[ -n "${TRANSFORM_BINARY:-}" ]] && rm -f "$TRANSFORM_BINARY"
    [[ -n "${TMPSPEC:-}" ]]          && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]]       || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]] || die "WORKLOAD_SPEC is not set."
    [[ -f "$WORKLOAD_SPEC" ]] || die "WORKLOAD_SPEC not found: $WORKLOAD_SPEC"
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    python3 -c "import pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user pandas numpy -q
    }
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: device '$DISK_DEVICE' not found in /proc/diskstats — disk I/O will be zero."
        log "  Available: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
    fi
    if [[ -z "$WORKLOAD_LABEL" ]]; then
        WORKLOAD_LABEL="$(basename "$WORKLOAD_SPEC" .spec)"
    fi
}

# ── Per-second system monitor (identical to saturation_sweep.sh) ──────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp "${SAT_TMPDIR}/slack2_monitor_XXXXX.py")
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
                return int(p[5]), int(p[9]), int(p[3]), int(p[7])
    return 0, 0, 0, 0

def read_mem_mib():
    info = {}
    with open('/proc/meminfo') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                info[parts[0].rstrip(':')] = int(parts[1])
    total = info.get('MemTotal', 0) / 1024
    avail = info.get('MemAvailable', 0) / 1024
    return total, avail, total - avail

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
                    f'{100*delta[0]/total:.2f}',
                    f'{100*delta[2]/total:.2f}',
                    f'{100*delta[4]/total:.2f}',
                    f'{100*delta[3]/total:.2f}',
                    f'{(cur_rd-prev_rd)*512/1024/1024/dt:.3f}',
                    f'{(cur_wr-prev_wr)*512/1024/1024/dt:.3f}',
                    f'{(cur_rc-prev_rc)/dt:.2f}',
                    f'{(cur_wc-prev_wc)/dt:.2f}',
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

# ── Compiled iBench_cpu worker ────────────────────────────────────────────────

compile_transform_binary() {
    local src_cc; src_cc="$(dirname "$0")/iBench_cpu.cc"
    [[ -f "$src_cc" ]] || die "iBench_cpu.cc not found at '$src_cc'"
    TRANSFORM_BINARY=$(mktemp "${SAT_TMPDIR}/slack2_cpu_XXXXX")
    log "Compiling iBench_cpu.cc → $TRANSFORM_BINARY"
    g++ -O2 -o "$TRANSFORM_BINARY" "$src_cc" -lpthread \
        || die "Failed to compile iBench_cpu.cc. Need g++ (no special flags required)."
    chmod +x "$TRANSFORM_BINARY"
}

# ── Page-cache flush ──────────────────────────────────────────────────────────

drop_page_cache() {
    if [[ "$DROP_CACHES" != "true" ]]; then return; fi
    log "  Dropping OS page cache (sync + drop_caches=3) ..."
    sudo sysctl -w vm.dirty_expire_centisecs=0 > /dev/null 2>&1 || true
    if echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
        log "  Page cache dropped."
    else
        log "  WARNING: drop_caches failed — disk reads may be served from RAM."
    fi
}

# ── Workload spec builder (mirrors saturation_sweep.sh exactly) ───────────────

create_spec() {
    TMPSPEC=$(mktemp "${SAT_TMPDIR}/slack2_spec_XXXXX.spec")
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    # Override recordcount only if RECORD_COUNT is explicitly set.
    if [[ -n "$RECORD_COUNT" ]]; then
        printf 'recordcount=%s\n'    "$RECORD_COUNT" >> "$TMPSPEC"
        printf 'operationcount=%s\n' "$RECORD_COUNT" >> "$TMPSPEC"
    fi
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase (mirrors saturation_sweep.sh load_db exactly) ─────────────────

load_db() {
    local dbpath="$1"
    # Optional second argument: path for the load log.
    # Defaults to OUTPUT_DIR/load.log for callers that do not need per-run logs.
    local logfile="${2:-$OUTPUT_DIR/load.log}"
    log "Loading ${RECORD_COUNT} records into $dbpath (~$(( RECORD_COUNT * 2064 / 1024 / 1024 / 1024 )) GiB) ..."
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap true -threads 8 \
        -load true -run false -throughput false \
        -runtime 0 \
        -levels 7 -table baseline \
        2>&1 | tee "$logfile"
    rm -f "$spec"; TMPSPEC=""
    log "Load complete."
}

# ── Single worker-count run ───────────────────────────────────────────────────
#
# run_one <n_workers> <run_dir> <dbpath>
#
# Mirrors saturation_sweep.sh run_one(), adding n_workers iBench_cpu workers
# started immediately after YCSB is backgrounded.  YCSB is the clock.

run_one() {
    local n_workers="$1" run_dir="$2" dbpath="$3"

    local sys_csv="$run_dir/system_w${n_workers}.csv"
    local log_file="$run_dir/ycsb_w${n_workers}.log"
    local cmp_csv="$run_dir/compaction_metrics.csv"

    local spec; spec=$(create_spec "metrics_output=${cmp_csv}")

    log "  Running: KNEE_THREADS=${KNEE_THREADS}  workers=${n_workers}  runtime=${RUNTIME_SECS}s" \
        "(skip first ${WARMUP_SKIP_S}s)"

    start_monitor "$sys_csv"

    # ── Start YCSB in the background ─────────────────────────────────────────
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$KNEE_THREADS" \
        -load false -run false -throughput true \
        -runtime "$RUNTIME_SECS" \
        -skip   "$WARMUP_SKIP_S" \
        -levels 7 -table baseline \
        -dbstatistics true \
        2>&1 | tee "$log_file" &
    local ycsb_pid=$!

    # ── Start n_workers iBench_cpu workers alongside YCSB ────────────────────
    # n_workers may be a float (e.g. 6.4 = 6 full cores + 0.4 fractional core).
    # bash (( )) only handles integers, so use awk for the float-safe guard.
    TRANSFORM_PIDS=()
    if awk "BEGIN { exit (${n_workers} > 0 ? 0 : 1) }"; then
        local rounds_per_xfm=$(( 1024 * WORKER_ROUNDS_MULTIPLIER ))
        # CPU affinity: pass the per-process base CPU ID when CPU_AFFINITY_BASE
        # is set; pass -1 otherwise (no pinning).
        local cpu_arg=-1
        if [[ -n "${CPU_AFFINITY_BASE:-}" ]]; then
            cpu_arg="${CPU_AFFINITY_BASE}"
        fi
        log "  Starting ${n_workers} iBench_cpu worker(s)" \
            "(rounds=${rounds_per_xfm}, rate=${TRANSFORMS_PER_WORKER}/s," \
            "cpu_base=${cpu_arg}, duration=${RUNTIME_SECS}s)"
        "$TRANSFORM_BINARY" \
            "$RUNTIME_SECS" "$n_workers" \
            "$TRANSFORMS_PER_WORKER" "$rounds_per_xfm" \
            "$cpu_arg" &
        TRANSFORM_PIDS=($!)
    fi

    # ── Wait for YCSB (it is the clock) ──────────────────────────────────────
    wait "$ycsb_pid" 2>/dev/null || true
    rm -f "$spec"; TMPSPEC=""

    # ── Cleanup workers and monitor ───────────────────────────────────────────
    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    TRANSFORM_PIDS=()
    stop_monitor

    log "  → log:        $log_file"
    log "  → sys:        $sys_csv"
    log "  → compaction: $cmp_csv"
}

# ── Main sweep (mirrors saturation_sweep.sh run_sweep exactly) ────────────────

run_sweep() {
    sep
    log "CPU slack sweep v2: $WORKLOAD_LABEL"
    log "  Spec:         $WORKLOAD_SPEC"
    log "  Knee threads: $KNEE_THREADS"
    log "  Workers:      $WORKER_COUNTS"
    log "  Runtime:      ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"
    local dbpath="$sweep_dir/rocksdb"

    for n_workers in $WORKER_COUNTS; do
        sep
        log "Workers: $n_workers  (${TRIALS_PER_POINT} trial(s))"
        local run_dir="$sweep_dir/run_w${n_workers}"
        mkdir -p "$run_dir"
        if (( TRIALS_PER_POINT == 1 )); then
            log "  Wiping DB and loading fresh dataset ..."
            rm -rf "$dbpath"
            load_db "$dbpath" "$run_dir/load.log"
            drop_page_cache
            run_one "$n_workers" "$run_dir" "$dbpath"
        else
            for (( trial=1; trial<=TRIALS_PER_POINT; trial++ )); do
                log "  Trial ${trial}/${TRIALS_PER_POINT}: wiping DB and loading fresh dataset ..."
                rm -rf "$dbpath"
                local trial_dir="$run_dir/trial_${trial}"
                mkdir -p "$trial_dir"
                load_db "$dbpath" "$trial_dir/load.log"
                drop_page_cache
                run_one "$n_workers" "$trial_dir" "$dbpath"
            done
        fi
    done

    log "Sweep complete."
}

# ── CSV summarizer ────────────────────────────────────────────────────────────

setup_summarizer_script() {
    SUMMARIZER_SCRIPT=$(mktemp "${SAT_TMPDIR}/slack2_summarize_XXXXX.py")
    cat > "$SUMMARIZER_SCRIPT" << 'PYSUM'
#!/usr/bin/env python3
"""
Aggregate per-worker-count raw files → summary.csv

Same schema as saturation_sweep.sh summary.csv, with 'workers' in place of
'threads'.  One row per worker count; all metrics are mean/std/min/max over
the steady-state window [WARMUP_SKIP_S, RUNTIME_SECS].
"""
import sys, os, re, glob
import pandas as pd
import numpy as np

sweep_dir      = sys.argv[1]
output_csv     = sys.argv[2]
warmup_skip    = int(sys.argv[3])
workload_label = sys.argv[4]
runtime_s      = int(sys.argv[5]) if len(sys.argv) > 5 else 600

nan = float('nan')

def worker_from_path(p):
    # Matches both integer ("run_w8") and fractional ("run_w6.4") directory names.
    m = re.search(r'_w([\d.]+)', os.path.basename(p))
    return float(m.group(1)) if m else 0.0

def fmt_w(w):
    """Reproduce bash's ${n_workers} literal so file-name lookups succeed.
    Whole numbers (0.0, 32.0) are formatted without the decimal point to match
    what bash writes; fractional values (6.4) include it."""
    return str(int(w)) if w == int(w) else str(w)

def parse_ycsb_log(path):
    """Return (xput_mean, xput_std, xput_min, xput_max, cache_hits, cache_misses)."""
    xput_mean = xput_std = xput_min = xput_max = nan
    cache_hits = cache_misses = nan
    try:
        text = open(path).read()
        m = re.search(r'throughput mean:\s*([\d.eE+\-]+)\s+stddev:\s*([\d.eE+\-]+)', text)
        if m:
            xput_mean, xput_std = float(m.group(1)), float(m.group(2))
        m_min = re.search(r'throughput\b.*?\bmin:\s*([\d.eE+\-]+)', text)
        m_max = re.search(r'throughput\b.*?\bmax:\s*([\d.eE+\-]+)', text)
        if m_min: xput_min = float(m_min.group(1))
        if m_max: xput_max = float(m_max.group(1))
        mh = re.search(r'rocksdb\.block\.cache\.hit\s+COUNT\s*:\s*(\d+)', text)
        mm = re.search(r'rocksdb\.block\.cache\.miss\s+COUNT\s*:\s*(\d+)', text)
        if mh: cache_hits   = int(mh.group(1))
        if mm: cache_misses = int(mm.group(1))
    except Exception as e:
        print(f'  [warn] {path}: {e}')
    return xput_mean, xput_std, xput_min, xput_max, cache_hits, cache_misses

def parse_system_csv(path, skip_s):
    empty = dict(
        cpu_compute_mean=nan, cpu_compute_std=nan,
        cpu_compute_min=nan,  cpu_compute_max=nan,
        cpu_schedule_mean=nan, cpu_schedule_std=nan,
        cpu_schedule_min=nan,  cpu_schedule_max=nan,
        disk_read_mean=nan,   disk_read_std=nan,
        disk_read_min=nan,    disk_read_max=nan,
        disk_write_mean=nan,  disk_write_std=nan,
        disk_write_min=nan,   disk_write_max=nan,
        disk_read_iops_mean=nan, disk_read_iops_std=nan,
        disk_read_iops_min=nan,  disk_read_iops_max=nan,
        disk_write_iops_mean=nan, disk_write_iops_std=nan,
        disk_write_iops_min=nan,  disk_write_iops_max=nan,
        mem_used_mean=nan, mem_used_std=nan,
        mem_used_min=nan,  mem_used_max=nan,
        mem_avail_mean=nan, mem_used_pct_mean=nan,
    )
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        # Clip to the intended measurement window [skip_s, runtime_s].
        # Lower bound removes warmup; upper bound removes the compaction-drain
        # tail that accumulates after YCSB finishes its timed window but before
        # RocksDB background threads stop writing to disk.
        steady = df[(df['elapsed_s'] >= skip_s) & (df['elapsed_s'] <= runtime_s)]
        if steady.empty: steady = df

        iowait = steady['cpu_iowait_pct'] if 'cpu_iowait_pct' in steady.columns \
                 else pd.Series(0.0, index=steady.index)
        cpu_schedule = 100.0 - steady['cpu_idle_pct']   # 1 - idle  (incl. iowait)
        cpu_compute  = cpu_schedule - iowait              # 1 - idle - iowait

        dr  = steady['disk_read_mbs']
        dw  = steady['disk_write_mbs']
        dri = steady['disk_read_iops']  if 'disk_read_iops'  in steady.columns \
              else pd.Series(dtype=float)
        dwi = steady['disk_write_iops'] if 'disk_write_iops' in steady.columns \
              else pd.Series(dtype=float)

        has_mem = 'mem_used_mib' in steady.columns
        mu = steady['mem_used_mib']  / 1024 if has_mem else pd.Series(dtype=float)
        ma = steady['mem_avail_mib'] / 1024 if has_mem else pd.Series(dtype=float)
        mt = steady['mem_total_mib'] / 1024 if has_mem else pd.Series(dtype=float)
        pct = (steady['mem_used_mib'] / steady['mem_total_mib'] * 100) if has_mem \
              else pd.Series(dtype=float)

        def s(series):
            return series if not series.empty else pd.Series(dtype=float)

        return dict(
            cpu_compute_mean  = cpu_compute.mean(),
            cpu_compute_std   = cpu_compute.std(ddof=1),
            cpu_compute_min   = cpu_compute.min(),
            cpu_compute_max   = cpu_compute.max(),
            cpu_schedule_mean = cpu_schedule.mean(),
            cpu_schedule_std  = cpu_schedule.std(ddof=1),
            cpu_schedule_min  = cpu_schedule.min(),
            cpu_schedule_max  = cpu_schedule.max(),
            disk_read_mean    = dr.mean(),  disk_read_std=dr.std(ddof=1),
            disk_read_min     = dr.min(),   disk_read_max=dr.max(),
            disk_write_mean   = dw.mean(),  disk_write_std=dw.std(ddof=1),
            disk_write_min    = dw.min(),   disk_write_max=dw.max(),
            disk_read_iops_mean  = s(dri).mean()      if not dri.empty else nan,
            disk_read_iops_std   = s(dri).std(ddof=1) if not dri.empty else nan,
            disk_read_iops_min   = s(dri).min()        if not dri.empty else nan,
            disk_read_iops_max   = s(dri).max()        if not dri.empty else nan,
            disk_write_iops_mean = s(dwi).mean()      if not dwi.empty else nan,
            disk_write_iops_std  = s(dwi).std(ddof=1) if not dwi.empty else nan,
            disk_write_iops_min  = s(dwi).min()        if not dwi.empty else nan,
            disk_write_iops_max  = s(dwi).max()        if not dwi.empty else nan,
            mem_used_mean     = s(mu).mean()      if not mu.empty else nan,
            mem_used_std      = s(mu).std(ddof=1) if not mu.empty else nan,
            mem_used_min      = s(mu).min()        if not mu.empty else nan,
            mem_used_max      = s(mu).max()        if not mu.empty else nan,
            mem_avail_mean    = s(ma).mean()      if not ma.empty else nan,
            mem_used_pct_mean = s(pct).mean()     if not pct.empty else nan,
        )
    except Exception as e:
        print(f'  [warn] {path}: {e}')
        return empty

def collect_trial(td, w):
    wl = fmt_w(w)   # e.g. 6.4 → "6.4",  8.0 → "8"
    xm, xs, xlo, xhi, ch, cm = parse_ycsb_log(os.path.join(td, f'ycsb_w{wl}.log'))
    ss = parse_system_csv(os.path.join(td, f'system_w{wl}.csv'), warmup_skip)
    return xm, xs, xlo, xhi, ch, cm, ss

run_dirs = sorted(
    glob.glob(os.path.join(sweep_dir, 'run_w*')),
    key=worker_from_path)

rows = []
for rd in run_dirs:
    w = worker_from_path(rd)

    trial_dirs = sorted(
        glob.glob(os.path.join(rd, 'trial_*')),
        key=lambda p: int(re.search(r'trial_(\d+)', os.path.basename(p)).group(1))
                      if re.search(r'trial_(\d+)', os.path.basename(p)) else 0)

    if trial_dirs:
        trial_xputs = []; trial_xmins = []; trial_xmaxs = []
        trial_sys = {}; trial_ch = []; trial_cm = []
        for td in trial_dirs:
            xm, _xs, xlo, xhi, ch, cm, ss = collect_trial(td, w)
            trial_xputs.append(xm); trial_xmins.append(xlo); trial_xmaxs.append(xhi)
            trial_ch.append(ch); trial_cm.append(cm)
            for k, v in ss.items():
                trial_sys.setdefault(k, []).append(v)
        xput_mean  = float(np.nanmedian(trial_xputs))
        xput_std   = float(np.nanstd(trial_xputs, ddof=1)) if len(trial_xputs) > 1 else nan
        xput_min   = float(np.nanmin(trial_xmins))  if trial_xmins else nan
        xput_max   = float(np.nanmax(trial_xmaxs))  if trial_xmaxs else nan
        cache_hits   = float(np.nanmedian(trial_ch))
        cache_misses = float(np.nanmedian(trial_cm))
        sys_stats  = {k: float(np.nanmedian(vs)) for k, vs in trial_sys.items()}
    else:
        wl = fmt_w(w)
        xput_mean, xput_std, xput_min, xput_max, cache_hits, cache_misses = \
            parse_ycsb_log(os.path.join(rd, f'ycsb_w{wl}.log'))
        sys_stats = parse_system_csv(os.path.join(rd, f'system_w{wl}.csv'), warmup_skip)

    total_c = (cache_hits + cache_misses) if not (np.isnan(cache_hits) or np.isnan(cache_misses)) else 0
    hit_rate = cache_hits / total_c if total_c > 0 else nan

    def fmt(v): return round(float(v), 2) if not np.isnan(v) else nan
    def ss(k):  return fmt(sys_stats.get(k, nan))

    rows.append(dict(
        workload_label        = workload_label,
        workers               = w,
        xput_mean             = fmt(xput_mean),
        xput_std              = fmt(xput_std),
        xput_min              = fmt(xput_min),
        xput_max              = fmt(xput_max),
        cpu_compute_mean      = ss('cpu_compute_mean'),
        cpu_compute_std       = ss('cpu_compute_std'),
        cpu_compute_min       = ss('cpu_compute_min'),
        cpu_compute_max       = ss('cpu_compute_max'),
        cpu_schedule_mean     = ss('cpu_schedule_mean'),
        cpu_schedule_std      = ss('cpu_schedule_std'),
        cpu_schedule_min      = ss('cpu_schedule_min'),
        cpu_schedule_max      = ss('cpu_schedule_max'),
        **{'disk_read_mb/s'   : ss('disk_read_mean')},
        **{'disk_read_mb/s_std': ss('disk_read_std')},
        **{'disk_read_mb/s_min': ss('disk_read_min')},
        **{'disk_read_mb/s_max': ss('disk_read_max')},
        **{'disk_write_mb/s'  : ss('disk_write_mean')},
        **{'disk_write_mb/s_std': ss('disk_write_std')},
        **{'disk_write_mb/s_min': ss('disk_write_min')},
        **{'disk_write_mb/s_max': ss('disk_write_max')},
        **{'r/s'              : ss('disk_read_iops_mean')},
        **{'r/s_std'          : ss('disk_read_iops_std')},
        **{'r/s_min'          : ss('disk_read_iops_min')},
        **{'r/s_max'          : ss('disk_read_iops_max')},
        **{'w/s'              : ss('disk_write_iops_mean')},
        **{'w/s_std'          : ss('disk_write_iops_std')},
        **{'w/s_min'          : ss('disk_write_iops_min')},
        **{'w/s_max'          : ss('disk_write_iops_max')},
        mem_used_mean         = ss('mem_used_mean'),
        mem_used_std          = ss('mem_used_std'),
        mem_used_min          = ss('mem_used_min'),
        mem_used_max          = ss('mem_used_max'),
        mem_avail_mean        = ss('mem_avail_mean'),
        mem_used_pct_mean     = ss('mem_used_pct_mean'),
        block_cache_hits      = cache_hits,
        block_cache_misses    = cache_misses,
        block_cache_hit_rate  = fmt(hit_rate),
    ))

if not rows:
    print('No run directories found — summary.csv not written.')
    sys.exit(0)

df = pd.DataFrame(rows).sort_values('workers').reset_index(drop=True)
df.to_csv(output_csv, index=False)
print(f'summary.csv written → {output_csv}')
preview_cols = ['workers', 'xput_mean', 'xput_std', 'xput_min', 'xput_max',
                'cpu_compute_mean', 'cpu_compute_std',
                'cpu_schedule_mean', 'cpu_schedule_std',
                'disk_read_mb/s', 'disk_write_mb/s', 'r/s', 'w/s',
                'mem_used_mean', 'block_cache_hit_rate']
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
        "$RUNTIME_SECS"
    log "  → $OUTPUT_DIR/summary.csv"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --knee-threads=*)     KNEE_THREADS="${1#*=}" ;;
            --knee-threads)       KNEE_THREADS="$2"; shift ;;
            --worker-counts=*)    WORKER_COUNTS="${1#*=}" ;;
            --worker-counts)      WORKER_COUNTS="$2"; shift ;;
            --runtime=*)          RUNTIME_SECS="${1#*=}" ;;
            --runtime)            RUNTIME_SECS="$2"; shift ;;
            --trials=*)           TRIALS_PER_POINT="${1#*=}" ;;
            --trials)             TRIALS_PER_POINT="$2"; shift ;;
            *) die "Unknown argument: $1" ;;
        esac
        shift
    done

    check_prereqs
    compile_transform_binary

    log "cpu_slack_sweep_v2.sh starting"
    log "  Binary:        $BINARY"
    log "  Output dir:    $OUTPUT_DIR"
    log "  Knee threads:  $KNEE_THREADS"
    log "  Worker counts: $WORKER_COUNTS"
    log "  Runtime/pt:    ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    log "  Trials/point:  ${TRIALS_PER_POINT}"
    log "  Device:        $DISK_DEVICE"
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
}

main "$@"
