#!/usr/bin/env bash
# =============================================================================
# cpu_slack_sweep_v1.sh — CPU intensity sweep (fixed workers, varying intensity)
#
# Fixes the number of iBench_cpu workers to COMPACTION_THREADS (a mandatory
# command-line argument) and sweeps their CPU intensity from 10 % to 100 % in
# 10 % steps.  At each intensity level we:
#   1. Wipe and reload a fresh RocksDB (same load path as saturation_sweep.sh)
#   2. Drop the OS page cache
#   3. Start YCSB in the background at KNEE_THREADS threads for RUNTIME_SECS
#   4. Immediately start COMPACTION_THREADS iBench_cpu workers rate-limited to
#      the target intensity fraction of their single-core saturation rate
#   5. Wait for YCSB → parse xput mean/std/min/max
#   6. Cleanup workers and monitor
#
# Intensity → rate mapping
# ────────────────────────
# Before the sweep, a tiny embedded C calibration binary is compiled and run
# for CAL_SECS seconds on a single thread to measure R_MAX (transforms/s at
# 100 % duty cycle with WORKER_ROUNDS rounds/transform).  Then:
#
#   intensity 100 % → rate = 0       (busy-loop, saturate one core)
#   intensity N   % → rate = round(N/100 × R_MAX)
#
# This produces data comparable to cpu_slack_sweep_v2.sh: v2 holds intensity
# at 100 % and sweeps worker count; v1 holds worker count at COMPACTION_THREADS
# and sweeps intensity.
#
# Run from the project BUILD directory:
#   cd /path/to/build
#   KNEE_THREADS=16 WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#     bash ../utility/cpu_slack_sweep_v1.sh --compaction-threads 4
#
# All env-var knobs can be overridden on the command line (long-option style)
# or by setting the variable before invoking the script.
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   sweep/
#     run_i<N>/             (N = intensity %, e.g. run_i10 … run_i100)
#       load.log
#       ycsb_i<N>.log
#       system_i<N>.csv
#       compaction_metrics.csv
#   summary.csv             — one row per intensity level; 'intensity_pct' col
# =============================================================================

set -euo pipefail

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./intensity_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"

# ── Workload ──────────────────────────────────────────────────────────────────
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"
WORKLOAD_LABEL="${WORKLOAD_LABEL:-}"

# ── Experiment knobs ──────────────────────────────────────────────────────────

# Fixed YCSB thread count (the saturation knee).
KNEE_THREADS="${KNEE_THREADS:-16}"

# Number of iBench_cpu workers.  MUST be supplied via --compaction-threads.
COMPACTION_THREADS="${COMPACTION_THREADS:-}"

# Intensity percentages to sweep (space-separated).
INTENSITY_LEVELS="${INTENSITY_LEVELS:-10 20 30 40 50 60 70 80 90 100}"

# Number of independent trials per intensity level (median is reported).
TRIALS_PER_POINT="${TRIALS_PER_POINT:-1}"

# Total experiment duration per point in seconds (= full YCSB runtime).
RUNTIME_SECS="${RUNTIME_SECS:-600}"

# Warmup seconds to discard from per-second system CSV when summarising.
WARMUP_SKIP_S="${WARMUP_SKIP_S:-0}"

# Records to preload.
RECORD_COUNT="${RECORD_COUNT:-10000000}"

# Block device to monitor (e.g. nvme0n1).
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Drop OS page cache before every run (requires passwordless sudo).
DROP_CACHES="${DROP_CACHES:-true}"

# ── iBench_cpu worker knobs ───────────────────────────────────────────────────

# mix64 rounds per transform.  Must match between the calibration run and the
# actual workers so that R_MAX correctly reflects the per-transform work.
# Default matches cpu_slack_sweep_v2.sh (WORKER_ROUNDS_MULTIPLIER=8 × 1024).
WORKER_ROUNDS="${WORKER_ROUNDS:-8192}"

# Calibration duration in seconds (short; just long enough for a stable count).
CAL_SECS="${CAL_SECS:-3}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
SUMMARIZER_SCRIPT=""
TRANSFORM_BINARY=""
CAL_BINARY=""
TMPSPEC=""
MONITOR_PID=""
R_MAX=0
declare -a TRANSFORM_PIDS=()

SAT_TMPDIR="/holly/htap/build"

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]]      && kill "$MONITOR_PID"   2>/dev/null || true
    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    [[ -n "${MONITOR_SCRIPT:-}" ]]    && rm -f "$MONITOR_SCRIPT"
    [[ -n "${SUMMARIZER_SCRIPT:-}" ]] && rm -f "$SUMMARIZER_SCRIPT"
    [[ -n "${TRANSFORM_BINARY:-}" ]]  && rm -f "$TRANSFORM_BINARY"
    [[ -n "${CAL_BINARY:-}" ]]        && rm -f "$CAL_BINARY"
    [[ -n "${TMPSPEC:-}" ]]           && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]]             || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]]      || die "WORKLOAD_SPEC is not set."
    [[ -f "$WORKLOAD_SPEC" ]]      || die "WORKLOAD_SPEC not found: $WORKLOAD_SPEC"
    [[ -n "$COMPACTION_THREADS" ]] || die "--compaction-threads is required."
    (( COMPACTION_THREADS > 0 ))   || die "--compaction-threads must be > 0."
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

# ── Per-second system monitor (identical to cpu_slack_sweep_v2.sh) ────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp "${SAT_TMPDIR}/intens1_monitor_XXXXX.py")
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
    TRANSFORM_BINARY=$(mktemp "${SAT_TMPDIR}/intens1_cpu_XXXXX")
    log "Compiling iBench_cpu.cc → $TRANSFORM_BINARY"
    g++ -O2 -fopenmp -o "$TRANSFORM_BINARY" "$src_cc" -lrt \
        || die "Failed to compile iBench_cpu.cc. Need g++ and OpenMP."
    chmod +x "$TRANSFORM_BINARY"
}

# ── Calibration binary ────────────────────────────────────────────────────────
#
# A minimal single-threaded mix64 counter.  Runs for CAL_SECS seconds, then
# prints the total transform count to stdout.  Compiled once; used by
# calibrate_rmax() to determine R_MAX so intensity percentages can be mapped
# to concrete iBench_cpu rate values.

compile_cal_binary() {
    local cal_src; cal_src=$(mktemp "${SAT_TMPDIR}/intens1_calsrc_XXXXX.cc")
    CAL_BINARY=$(mktemp "${SAT_TMPDIR}/intens1_cal_XXXXX")

    cat > "$cal_src" << 'CALSRC'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const uint64_t M1 = 0xFF51AFD7ED558CCDULL;
static const uint64_t M2 = 0xC4CEB9FE1A85EC53ULL;
#define NS_PER_S 1000000000ULL

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= M1;
    x ^= x >> 33; x *= M2;
    x ^= x >> 33;
    return x;
}

static inline uint64_t getNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    int      secs   = (argc >= 2) ? atoi(argv[1]) : 2;
    uint32_t rounds = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 512;

    uint64_t endNs = getNs() + (uint64_t)secs * NS_PER_S;
    uint64_t count = 0;
    uint64_t state = 0xDEADBEEFCAFEBABEULL;
    volatile uint64_t sink = 0;

    while (1) {
        uint64_t x = state;
        uint32_t r;
        for (r = 0; r < rounds; r++) x = mix64(x);
        state = x;
        count++;
        /* Check time every 64 K transforms to amortise clock_gettime cost. */
        if ((count & 0xFFFFULL) == 0 && getNs() >= endNs) break;
    }
    sink = state;
    (void)sink;
    printf("%llu\n", (unsigned long long)count);
    return 0;
}
CALSRC

    log "Compiling calibration binary → $CAL_BINARY"
    g++ -O2 -o "$CAL_BINARY" "$cal_src" -lrt \
        || die "Failed to compile calibration binary."
    chmod +x "$CAL_BINARY"
    rm -f "$cal_src"
}

# ── Measure single-thread saturation rate ─────────────────────────────────────

calibrate_rmax() {
    log "Calibrating single-thread mix64 rate (rounds=${WORKER_ROUNDS}, ${CAL_SECS}s) ..."
    local count
    count=$("$CAL_BINARY" "$CAL_SECS" "$WORKER_ROUNDS")
    R_MAX=$(( count / CAL_SECS ))
    log "  R_MAX = ${R_MAX} transforms/s  (1 thread, ${WORKER_ROUNDS} rounds/transform)"
    (( R_MAX > 0 )) || die "Calibration returned 0 transforms — check compilation."
}

# ── Map intensity percentage to iBench_cpu rate argument ─────────────────────
#
# intensity_to_rate <pct>
#   100 % → 0         (rate=0: busy-loop, saturate one core)
#   N   % → round(N/100 × R_MAX)  (rate-limited so N % of a core is consumed)

intensity_to_rate() {
    local pct="$1"
    if (( pct >= 100 )); then
        echo 0
    else
        # Integer rounding: (pct * R_MAX + 50) / 100, minimum 1.
        local r=$(( (pct * R_MAX + 50) / 100 ))
        echo $(( r > 0 ? r : 1 ))
    fi
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

# ── Workload spec builder ─────────────────────────────────────────────────────

create_spec() {
    TMPSPEC=$(mktemp "${SAT_TMPDIR}/intens1_spec_XXXXX.spec")
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    if [[ -n "$RECORD_COUNT" ]]; then
        printf 'recordcount=%s\n'    "$RECORD_COUNT" >> "$TMPSPEC"
        printf 'operationcount=%s\n' "$RECORD_COUNT" >> "$TMPSPEC"
    fi
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────

load_db() {
    local dbpath="$1"
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

# ── Single intensity-level run ────────────────────────────────────────────────
#
# run_one <intensity_pct> <run_dir> <dbpath>
#
# Launches YCSB at KNEE_THREADS and COMPACTION_THREADS iBench_cpu workers
# rate-limited to <intensity_pct>% of their single-core saturation rate.
# YCSB is the clock; workers are killed/waited after YCSB finishes.

run_one() {
    local intensity="$1" run_dir="$2" dbpath="$3"
    local rate; rate=$(intensity_to_rate "$intensity")

    local sys_csv="$run_dir/system_i${intensity}.csv"
    local log_file="$run_dir/ycsb_i${intensity}.log"
    local cmp_csv="$run_dir/compaction_metrics.csv"

    local spec; spec=$(create_spec "metrics_output=${cmp_csv}")

    if (( rate == 0 )); then
        log "  Running: KNEE_THREADS=${KNEE_THREADS}  compaction_threads=${COMPACTION_THREADS}" \
            "intensity=${intensity}%  (busy-loop)  runtime=${RUNTIME_SECS}s" \
            "(skip first ${WARMUP_SKIP_S}s)"
    else
        log "  Running: KNEE_THREADS=${KNEE_THREADS}  compaction_threads=${COMPACTION_THREADS}" \
            "intensity=${intensity}%  rate=${rate}/s  runtime=${RUNTIME_SECS}s" \
            "(skip first ${WARMUP_SKIP_S}s)"
    fi

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

    # ── Start COMPACTION_THREADS iBench_cpu workers at target intensity ───────
    TRANSFORM_PIDS=()
    log "  Starting ${COMPACTION_THREADS} iBench_cpu worker(s)" \
        "(rounds=${WORKER_ROUNDS}, rate=${rate}/s, duration=${RUNTIME_SECS}s)"
    "$TRANSFORM_BINARY" \
        "$RUNTIME_SECS" "$COMPACTION_THREADS" \
        "$rate" "$WORKER_ROUNDS" &
    TRANSFORM_PIDS=($!)

    # ── Wait for YCSB (it is the clock) ──────────────────────────────────────
    wait "$ycsb_pid" 2>/dev/null || true
    rm -f "$spec"; TMPSPEC=""

    # ── Cleanup workers and monitor ───────────────────────────────────────────
    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    TRANSFORM_PIDS=()
    stop_monitor

    log "  → log:        $log_file"
    log "  → sys:        $sys_csv"
    log "  → compaction: $cmp_csv"
}

# ── Main intensity sweep ──────────────────────────────────────────────────────

run_sweep() {
    sep
    log "CPU intensity sweep v1: $WORKLOAD_LABEL"
    log "  Spec:               $WORKLOAD_SPEC"
    log "  Knee threads:       $KNEE_THREADS"
    log "  Compaction threads: $COMPACTION_THREADS"
    log "  Intensity levels:   $INTENSITY_LEVELS"
    log "  R_MAX (1 thread):   $R_MAX transforms/s  (rounds=${WORKER_ROUNDS})"
    log "  Runtime:            ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"
    local dbpath="$sweep_dir/rocksdb"

    for intensity in $INTENSITY_LEVELS; do
        sep
        log "Intensity: ${intensity}%  (rate=$(intensity_to_rate "$intensity")/s)  ${TRIALS_PER_POINT} trial(s)"
        local run_dir="$sweep_dir/run_i${intensity}"
        mkdir -p "$run_dir"

        if (( TRIALS_PER_POINT == 1 )); then
            log "  Wiping DB and loading fresh dataset ..."
            rm -rf "$dbpath"
            load_db "$dbpath" "$run_dir/load.log"
            drop_page_cache
            run_one "$intensity" "$run_dir" "$dbpath"
        else
            for (( trial=1; trial<=TRIALS_PER_POINT; trial++ )); do
                log "  Trial ${trial}/${TRIALS_PER_POINT}: wiping DB and loading fresh dataset ..."
                rm -rf "$dbpath"
                local trial_dir="$run_dir/trial_${trial}"
                mkdir -p "$trial_dir"
                load_db "$dbpath" "$trial_dir/load.log"
                drop_page_cache
                run_one "$intensity" "$trial_dir" "$dbpath"
            done
        fi
    done

    log "Sweep complete."
}

# ── CSV summarizer ────────────────────────────────────────────────────────────

setup_summarizer_script() {
    SUMMARIZER_SCRIPT=$(mktemp "${SAT_TMPDIR}/intens1_summarize_XXXXX.py")
    cat > "$SUMMARIZER_SCRIPT" << 'PYSUM'
#!/usr/bin/env python3
"""
Aggregate per-intensity-level raw files → summary.csv

Same schema as cpu_slack_sweep_v2.sh summary.csv, with 'intensity_pct' in
place of 'workers'.  One row per intensity level; metrics are mean/std/min/max
over the steady-state window [WARMUP_SKIP_S, RUNTIME_SECS].
"""
import sys, os, re, glob
import pandas as pd
import numpy as np

sweep_dir      = sys.argv[1]
output_csv     = sys.argv[2]
warmup_skip    = int(sys.argv[3])
workload_label = sys.argv[4]

nan = float('nan')

def intensity_from_path(p):
    m = re.search(r'_i(\d+)', os.path.basename(p))
    return int(m.group(1)) if m else 0

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
        steady = df[df['elapsed_s'] >= skip_s]
        if steady.empty: steady = df

        iowait = steady['cpu_iowait_pct'] if 'cpu_iowait_pct' in steady.columns \
                 else pd.Series(0.0, index=steady.index)
        cpu_schedule = 100.0 - steady['cpu_idle_pct']
        cpu_compute  = cpu_schedule - iowait

        dr  = steady['disk_read_mbs']
        dw  = steady['disk_write_mbs']
        dri = steady['disk_read_iops']  if 'disk_read_iops'  in steady.columns \
              else pd.Series(dtype=float)
        dwi = steady['disk_write_iops'] if 'disk_write_iops' in steady.columns \
              else pd.Series(dtype=float)

        has_mem = 'mem_used_mib' in steady.columns
        mu  = steady['mem_used_mib']  / 1024 if has_mem else pd.Series(dtype=float)
        ma  = steady['mem_avail_mib'] / 1024 if has_mem else pd.Series(dtype=float)
        mt  = steady['mem_total_mib'] / 1024 if has_mem else pd.Series(dtype=float)
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

def collect_trial(td, i):
    xm, xs, xlo, xhi, ch, cm = parse_ycsb_log(os.path.join(td, f'ycsb_i{i}.log'))
    ss = parse_system_csv(os.path.join(td, f'system_i{i}.csv'), warmup_skip)
    return xm, xs, xlo, xhi, ch, cm, ss

run_dirs = sorted(
    glob.glob(os.path.join(sweep_dir, 'run_i*')),
    key=intensity_from_path)

rows = []
for rd in run_dirs:
    i = intensity_from_path(rd)

    trial_dirs = sorted(
        glob.glob(os.path.join(rd, 'trial_*')),
        key=lambda p: int(re.search(r'trial_(\d+)', os.path.basename(p)).group(1))
                      if re.search(r'trial_(\d+)', os.path.basename(p)) else 0)

    if trial_dirs:
        trial_xputs = []; trial_xmins = []; trial_xmaxs = []
        trial_sys = {}; trial_ch = []; trial_cm = []
        for td in trial_dirs:
            xm, _xs, xlo, xhi, ch, cm, ss = collect_trial(td, i)
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
        xput_mean, xput_std, xput_min, xput_max, cache_hits, cache_misses = \
            parse_ycsb_log(os.path.join(rd, f'ycsb_i{i}.log'))
        sys_stats = parse_system_csv(os.path.join(rd, f'system_i{i}.csv'), warmup_skip)

    total_c = (cache_hits + cache_misses) if not (np.isnan(cache_hits) or np.isnan(cache_misses)) else 0
    hit_rate = cache_hits / total_c if total_c > 0 else nan

    def fmt(v): return round(float(v), 2) if not np.isnan(v) else nan
    def ss(k):  return fmt(sys_stats.get(k, nan))

    rows.append(dict(
        workload_label        = workload_label,
        intensity_pct         = i,
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
        **{'disk_read_mb/s'    : ss('disk_read_mean')},
        **{'disk_read_mb/s_std': ss('disk_read_std')},
        **{'disk_read_mb/s_min': ss('disk_read_min')},
        **{'disk_read_mb/s_max': ss('disk_read_max')},
        **{'disk_write_mb/s'   : ss('disk_write_mean')},
        **{'disk_write_mb/s_std': ss('disk_write_std')},
        **{'disk_write_mb/s_min': ss('disk_write_min')},
        **{'disk_write_mb/s_max': ss('disk_write_max')},
        **{'r/s'               : ss('disk_read_iops_mean')},
        **{'r/s_std'           : ss('disk_read_iops_std')},
        **{'r/s_min'           : ss('disk_read_iops_min')},
        **{'r/s_max'           : ss('disk_read_iops_max')},
        **{'w/s'               : ss('disk_write_iops_mean')},
        **{'w/s_std'           : ss('disk_write_iops_std')},
        **{'w/s_min'           : ss('disk_write_iops_min')},
        **{'w/s_max'           : ss('disk_write_iops_max')},
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

df = pd.DataFrame(rows).sort_values('intensity_pct').reset_index(drop=True)
df.to_csv(output_csv, index=False)
print(f'summary.csv written → {output_csv}')
preview_cols = ['intensity_pct', 'xput_mean', 'xput_std', 'xput_min', 'xput_max',
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
        "$WORKLOAD_LABEL"
    log "  → $OUTPUT_DIR/summary.csv"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --compaction-threads=*) COMPACTION_THREADS="${1#*=}" ;;
            --compaction-threads)   COMPACTION_THREADS="$2"; shift ;;
            --knee-threads=*)       KNEE_THREADS="${1#*=}" ;;
            --knee-threads)         KNEE_THREADS="$2"; shift ;;
            --intensity-levels=*)   INTENSITY_LEVELS="${1#*=}" ;;
            --intensity-levels)     INTENSITY_LEVELS="$2"; shift ;;
            --worker-rounds=*)      WORKER_ROUNDS="${1#*=}" ;;
            --worker-rounds)        WORKER_ROUNDS="$2"; shift ;;
            --runtime=*)            RUNTIME_SECS="${1#*=}" ;;
            --runtime)              RUNTIME_SECS="$2"; shift ;;
            --trials=*)             TRIALS_PER_POINT="${1#*=}" ;;
            --trials)               TRIALS_PER_POINT="$2"; shift ;;
            --cal-secs=*)           CAL_SECS="${1#*=}" ;;
            --cal-secs)             CAL_SECS="$2"; shift ;;
            *) die "Unknown argument: $1" ;;
        esac
        shift
    done

    check_prereqs
    compile_transform_binary
    compile_cal_binary
    calibrate_rmax

    log "cpu_slack_sweep_v1.sh starting"
    log "  Binary:             $BINARY"
    log "  Output dir:         $OUTPUT_DIR"
    log "  Knee threads:       $KNEE_THREADS"
    log "  Compaction threads: $COMPACTION_THREADS"
    log "  Intensity levels:   $INTENSITY_LEVELS"
    log "  Worker rounds:      $WORKER_ROUNDS"
    log "  R_MAX:              $R_MAX transforms/s/thread"
    log "  Runtime/pt:         ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    log "  Trials/point:       ${TRIALS_PER_POINT}"
    log "  Device:             $DISK_DEVICE"
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
