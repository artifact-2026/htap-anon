#!/usr/bin/env bash
# =============================================================================
# cpu_slack_sweep.sh — CPU slack characterization (single-run incremental sweep)
#
# Story: create a fresh RocksDB, preload it (same as the saturation experiment),
# then start a single 900-second YCSB throughput run at KNEE_THREADS.  Every
# XPUT_WINDOW seconds (default 30) one additional rate-limited CPU transform
# worker is launched alongside the running YCSB client.
#
# The YCSB binary reports per-window throughput (mean + stddev) via -xputwindow
# and -xputfile into a CSV.  Each row in that CSV corresponds to one 30-second
# interval; the row index tells you how many workers were active.
#
#   window 0  (t=  0..30): 0 workers  — pure-YCSB baseline
#   window 1  (t= 30..60): 1 worker
#   window 2  (t= 60..90): 2 workers
#   …
#
# The claim produced: "baseline absorbs up to K transform workers before write
# throughput degrades more than DEGRADATION_THRESHOLD."
#
# Run from the project BUILD directory:
#   cd /path/to/build && \
#     KNEE_THREADS=16 WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#     bash ../utility/cpu_slack_sweep.sh
#
# Minimum required configuration:
#   KNEE_THREADS=<N>        client write threads at the knee (required)
#   WORKLOAD_SPEC=<path>    YCSB workload spec file (required)
#
# Optional overrides:
#   KNEE_XPUT=<ops/s>       knee throughput for plot annotation (0 = auto)
#   RECORD_COUNT=<N>        records to load (default 5M)
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   db/                        — freshly loaded RocksDB
#   sweep/
#     load.log                 — load-phase output
#     ycsb.log                 — single YCSB throughput run
#     xput_windows.csv         — per-window throughput: window_start_sec, avg, stddev
#     system.csv               — per-second CPU/disk samples
#     transform_w<K>.csv       — per-second transform counts for worker K
#   plots/
#     cpu_slack_curve.png      — throughput + CPU over time, annotated with workers
#     cpu_slack_summary.csv    — machine-readable per-window summary
# =============================================================================

set -euo pipefail

# ── Optional: path to saturation_sweep.sh summary.csv ─────────────────────────
# When provided, the combined plotter overlays saturation-phase CPU and disk
# utilization alongside the slack-sweep phase in a single chart.
# X-axis ticks: 1, 2, …, KNEE_THREADS, KNEE_THREADS+1, … , KNEE_THREADS+N.
SATURATION_SUMMARY="${SATURATION_SUMMARY:-}"

# ── Optional: source Phase 1 calibration file ─────────────────────────────────
# Run calibrate_transform_rate.sh first, then either:
#
#   source /path/to/sat_results_*/phase2_config.env
#   bash ../utility/cpu_slack_sweep.sh
#
# or pass the file explicitly:
#   PHASE2_CONFIG=/path/to/phase2_config.env bash ../utility/cpu_slack_sweep.sh
#
# The env file pre-populates KNEE_THREADS, KNEE_XPUT, TRANSFORMS_PER_WORKER,
# TRANSFORM_WORKER_COUNTS, FIELD_COUNT, and FIELD_LENGTH from measured data.
if [[ -n "${PHASE2_CONFIG:-}" ]]; then
    [[ -f "$PHASE2_CONFIG" ]] || {
        echo "ERROR: PHASE2_CONFIG not found: $PHASE2_CONFIG" >&2; exit 1; }
    # shellcheck source=/dev/null
    source "$PHASE2_CONFIG"
    echo "[$(date '+%H:%M:%S')] Sourced Phase 1 calibration: $PHASE2_CONFIG"
fi

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./slack_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── DB setup ──────────────────────────────────────────────────────────────────

# A fresh RocksDB is always created under OUTPUT_DIR/db and loaded from scratch.
# Number of records to load.
RECORD_COUNT="${RECORD_COUNT:-5000000}"

# ── Workload ──────────────────────────────────────────────────────────────────
# WORKLOAD_SPEC: path to a YCSB .spec file (required — no default).
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"

# Number of YCSB writer threads at the knee (from saturation_sweep.sh output).
KNEE_THREADS="${KNEE_THREADS:-16}"

# Knee throughput in ops/sec (from Phase 1 / calibrate_transform_rate.sh).
# Used as the degradation baseline — no Step 0 measurement is performed.
KNEE_XPUT="${KNEE_XPUT:-0}"

# ── Value layout — must match saturation_sweep.sh ─────────────────────────────

FIELD_LENGTH="${FIELD_LENGTH:-256}"
FIELD_COUNT="${FIELD_COUNT:-16}"

# ── Transform worker knobs ────────────────────────────────────────────────────

# Target transform rate per worker thread (transforms / second).
# Each transform performs field_count × (field_length / 8) Murmur3-style integer
# mix rounds — pure CPU register work, no memory or disk I/O.
# Set relative to your measured compaction output rate from Phase 1.
TRANSFORMS_PER_WORKER="${TRANSFORMS_PER_WORKER:-1000}"

# CPU intensity multiplier — scales the number of arithmetic rounds per
# transform without changing the logical rate (transforms/sec).
#
# Duty cycle per worker ≈ (ROUNDS_PER_TRANSFORM × ~300 ns) × rate.
# With the defaults (512 rounds, 1000 Hz) that is only ~15 %, meaning each
# worker sleeps ~85 % of the time and barely registers on CPU.
#
# The crossover to a busy loop (100 % duty cycle) happens when:
#   WORKER_ROUNDS_MULTIPLIER ≥ 1 / (base_rounds × ~300 ns × rate)
#                            ≈ 1 / (512 × 300e-9 × 1000) ≈ 6.5
#
# Recommended starting points:
#   4  → ~60 % duty cycle per worker  (light pressure)
#   8  → ~100 % duty cycle (saturates one core per worker; sweet spot)
#   16 → 100 % duty cycle with head room for faster hardware
# Values beyond ~8 have no additional effect once the worker is a busy loop.
WORKER_ROUNDS_MULTIPLIER="${WORKER_ROUNDS_MULTIPLIER:-8}"

# ── Experiment timing ─────────────────────────────────────────────────────────

# Block device to monitor for disk I/O (passed to the system monitor).
# Leave unset to auto-detect from the filesystem OUTPUT_DIR lives on.
# Override explicitly when the data disk differs from the OS disk, e.g.:
#   DISK_DEVICE=sdb bash ../utility/cpu_slack_sweep.sh
DISK_DEVICE="${DISK_DEVICE:-}"

# Total seconds for the single YCSB throughput run.
# With XPUT_WINDOW=30 this yields 30 per-window measurements: window 0 is the
# pure-YCSB baseline (0 workers) and each subsequent window adds one more worker.
RUNTIME_SECS="${RUNTIME_SECS:-900}"

# Per-window duration in seconds.  One new CPU transform worker is launched at
# the start of each window after window 0.  Must divide evenly into RUNTIME_SECS.
# Passed to the YCSB binary as -xputwindow so the per-window CSV rows align with
# the worker schedule.
XPUT_WINDOW="${XPUT_WINDOW:-30}"

# Warmup seconds to discard from the per-second system-monitor CSV when plotting.
# Set to 0 because window 0 already serves as the no-worker baseline.
WARMUP_SKIP_S="${WARMUP_SKIP_S:-0}"

# Throughput degradation threshold used for slack-boundary annotation in the plot.
# Does NOT stop the run early — the single YCSB run always completes RUNTIME_SECS.
DEGRADATION_THRESHOLD="${DEGRADATION_THRESHOLD:-0.10}"

# Drop the OS page cache before every YCSB run so that disk reads are not
# silently served from RAM.  Must match saturation_sweep.sh's DROP_CACHES
# behaviour so that KNEE_XPUT (measured cold) is a valid baseline here.
# Requires passwordless sudo for tee /proc/sys/vm/drop_caches, or root.
# Set to false only if you deliberately want a warm-cache Phase 2 and will
# supply a matching warm-cache KNEE_XPUT (e.g. from a Step 0 run).
DROP_CACHES="${DROP_CACHES:-true}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
TRANSFORM_BINARY=""     # compiled iBench_cpu binary (replaces Python worker)
PLOTTER_SCRIPT=""
COMBINED_PLOTTER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""
# Client write throughput (ops/s) and per-second stddev parsed from YCSB output.
BASELINE_XPUT_OPS=""
LAST_XPUT_OPS=""
LAST_XPUT_STDDEV=""
declare -a TRANSFORM_PIDS=()

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}"      ]] && kill "$MONITOR_PID"      2>/dev/null || true
    # Kill any transform workers still running (e.g., on early exit).
    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    [[ -n "${MONITOR_SCRIPT:-}"          ]] && rm -f "$MONITOR_SCRIPT"
    [[ -n "${TRANSFORM_BINARY:-}"        ]] && rm -f "$TRANSFORM_BINARY"
    [[ -n "${PLOTTER_SCRIPT:-}"          ]] && rm -f "$PLOTTER_SCRIPT"
    [[ -n "${COMBINED_PLOTTER_SCRIPT:-}" ]] && rm -f "$COMBINED_PLOTTER_SCRIPT"
    [[ -n "${TMPSPEC:-}"                 ]] && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]] || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC is not set.  Point it at a .spec file."
    [[ -f "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC file not found: $WORKLOAD_SPEC"
    # KNEE_XPUT is optional: 0 means no baseline annotation in the plot.
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    python3 -c "import matplotlib, pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user matplotlib pandas numpy -q \
            || sudo apt-get install -y python3-matplotlib python3-pandas python3-numpy -q
    }
    [[ -d "$WORKLOAD_DIR" ]] || die "Workload dir not found: $WORKLOAD_DIR"
    # Auto-detect DISK_DEVICE if unset or not present in /proc/diskstats.
    # Override with DISK_DEVICE=sdb (or whichever device) when the data disk
    # differs from the OS disk (e.g. multi-disk setups).
    if [[ -z "$DISK_DEVICE" ]] || \
       ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        local devfile devname detected=""
        # Ask df which block device backs the directory OUTPUT_DIR will live in.
        devfile=$(df "$(dirname "$OUTPUT_DIR")" 2>/dev/null | awk 'NR==2{print $1}')
        devname="${devfile##*/}"   # strip /dev/ prefix
        # Try candidates in order: as-is, strip NVMe partition suffix (p1→""),
        # strip SATA partition suffix (trailing digit: sda1→sda).
        local c
        for c in "$devname" \
                 "$(echo "$devname" | sed -E 's/p[0-9]+$//')" \
                 "$(echo "$devname" | sed -E 's/[0-9]+$//')"; do
            if [[ -n "$c" ]] && \
               awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$c"; then
                detected="$c"; break
            fi
        done
        if [[ -n "$detected" ]]; then
            log "  DISK_DEVICE '$DISK_DEVICE' not found; auto-detected '$detected' from $(dirname "$OUTPUT_DIR")"
            DISK_DEVICE="$detected"
        else
            log "WARNING: DISK_DEVICE '$DISK_DEVICE' not found and auto-detection failed — disk I/O will be zero."
            log "  Set DISK_DEVICE manually. Available: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
        fi
    fi
    (( RUNTIME_SECS > 0 ))   || die "RUNTIME_SECS must be > 0"
    (( XPUT_WINDOW > 0 ))    || die "XPUT_WINDOW must be > 0"
    (( RUNTIME_SECS % XPUT_WINDOW == 0 )) || \
        die "RUNTIME_SECS ($RUNTIME_SECS) must be divisible by XPUT_WINDOW ($XPUT_WINDOW)"
}

# ── Embedded per-second system monitor ───────────────────────────────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp /tmp/slack_monitor_XXXXX.py)
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
"""Per-second CPU and disk I/O monitor."""
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    # Returns fields 1-8 of the first 'cpu' line in /proc/stat:
    # user nice sys idle iowait irq softirq steal
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]

def read_ctxt():
    # Total context switches since boot; diff gives ctx/sec.
    with open('/proc/stat') as f:
        for line in f:
            if line.startswith('ctxt '):
                return int(line.split()[1])
    return 0

def read_disk(dev):
    if not dev:
        return 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                return int(p[5]), int(p[9])
    return 0, 0

prev_cpu  = read_cpu()
prev_ctxt = read_ctxt()
prev_rd, prev_wr = read_disk(disk_device)
prev_t   = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s', 'cpu_user_pct', 'cpu_sys_pct',
                'cpu_iowait_pct', 'cpu_idle_pct',
                'disk_read_mbs', 'disk_write_mbs',
                'ctx_switches_per_sec'])
    fh.flush()
    while True:
        time.sleep(1)
        now      = time.time()
        cur_cpu  = read_cpu()
        cur_ctxt = read_ctxt()
        cur_rd, cur_wr = read_disk(disk_device)

        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        idle_pct   = 100.0 * delta[3] / total
        user_pct   = 100.0 * delta[0] / total
        sys_pct    = 100.0 * delta[2] / total
        iowait_pct = 100.0 * delta[4] / total
        dt = (now - prev_t) or 1
        rd_mbs    = (cur_rd   - prev_rd)   * 512 / 1024 / 1024 / dt
        wr_mbs    = (cur_wr   - prev_wr)   * 512 / 1024 / 1024 / dt
        ctx_per_s = (cur_ctxt - prev_ctxt) / dt

        w.writerow([int(now),
                    f'{user_pct:.2f}', f'{sys_pct:.2f}',
                    f'{iowait_pct:.2f}', f'{idle_pct:.2f}',
                    f'{rd_mbs:.3f}', f'{wr_mbs:.3f}',
                    f'{ctx_per_s:.0f}'])
        fh.flush()
        prev_cpu  = cur_cpu
        prev_ctxt = cur_ctxt
        prev_rd, prev_wr = cur_rd, cur_wr
        prev_t   = now
PYMON
}

# ── Compiled transform worker (iBench_cpu.cc) ────────────────────────────────
#
# Each worker is a separate process running the compiled iBench_cpu binary.
# The binary performs Murmur3-style 64-bit integer mix rounds with optional
# rate limiting — pure CPU register work, no memory or disk I/O.
#
# Using C++ instead of Python eliminates interpreter overhead (~100×) and
# gives precise, low-jitter per-core CPU pressure via clock_nanosleep.

compile_transform_binary() {
    local src_cc; src_cc="$(dirname "$0")/iBench_cpu.cc"
    [[ -f "$src_cc" ]] || die "iBench_cpu.cc not found at '$src_cc'"
    TRANSFORM_BINARY=$(mktemp /tmp/slack_cpu_XXXXX)
    log "Compiling iBench_cpu.cc → $TRANSFORM_BINARY"
    g++ -O2 -fopenmp -o "$TRANSFORM_BINARY" "$src_cc" -lrt \
        || die "Failed to compile iBench_cpu.cc.  Need g++ and OpenMP."
    chmod +x "$TRANSFORM_BINARY"
}

# ── Monitor / transform helpers ───────────────────────────────────────────────

start_monitor()   { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor()    {
    [[ -n "${MONITOR_PID:-}" ]] && {
        kill "$MONITOR_PID" 2>/dev/null || true; wait "$MONITOR_PID" 2>/dev/null || true; MONITOR_PID=""; }
}

# ── Page-cache flush ──────────────────────────────────────────────────────────
# Without this, the kernel serves RocksDB reads from the OS page cache, which
# inflates throughput above the cold-cache knee measured by saturation_sweep.sh
# and makes KNEE_XPUT an invalid degradation baseline.
drop_page_cache() {
    if [[ "$DROP_CACHES" != "true" ]]; then
        return
    fi
    log "  Dropping OS page cache (sync + drop_caches=3) ..."
    sudo sysctl -w vm.dirty_expire_centisecs=0 > /dev/null 2>&1 || true
    if echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
        log "  Page cache dropped."
    else
        log "  WARNING: drop_caches failed (need sudo or root). Reads may be served from RAM."
        log "           Re-run as root or grant passwordless sudo for tee /proc/sys/vm/drop_caches."
    fi
}

# ── Workload spec ─────────────────────────────────────────────────────────────

create_spec() {
    TMPSPEC=$(mktemp /tmp/slack_spec_XXXXX.spec)
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────
#
# load_db <dbpath> [logfile]
#
# Creates a fresh RocksDB at <dbpath> and loads RECORD_COUNT records from the
# workload spec.  Mirrors load_db() in sands/saturation_sweep.sh exactly so
# that the pre-loaded state matches the saturation experiment.

load_db() {
    local dbpath="$1"
    local logfile="${2:-$OUTPUT_DIR/load.log}"
    local rc; rc=$(grep -E '^recordcount\s*=' "$WORKLOAD_SPEC" | tail -1 | cut -d= -f2 | tr -d ' ')
    rc="${rc:-$RECORD_COUNT}"
    log "Loading ${rc} records into $dbpath ..."
    mkdir -p "$dbpath"
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


# ── Single transform-worker-count run ─────────────────────────────────────────
#
# run_one <n_workers> <run_dir> <dbpath>
#
# Mirrors run_one() from saturation_sweep.sh, but with n_workers in-memory
# CRC32 transform workers running concurrently alongside YCSB.
#
# Flow:
#   0. Drop the OS page cache (same as saturation_sweep.sh) so reads are not
#      silently served from RAM; keeps KNEE_XPUT a valid cold-cache baseline.
#   1. Start n_workers transform workers (duration = RUNTIME_SECS).
#   2. Start the per-second system monitor.
#   3. Run the YCSB binary: -throughput true -runtime RUNTIME_SECS.
#      The binary internally skips the first WARMUP_SKIP_S (60) seconds and
#      reports the mean/stddev over [60, RUNTIME_SECS].
#   4. Wait for YCSB to finish (it exits after RUNTIME_SECS seconds).
#   5. Wait for transform workers to finish (they also run for RUNTIME_SECS).
#   6. Stop the monitor.
#   7. Parse LAST_XPUT_OPS from the YCSB log.

run_one() {
    local n_workers="$1" run_dir="$2" dbpath="$3"

    local sys_csv="$run_dir/system_w${n_workers}.csv"
    local xfm_csv="$run_dir/transform_w${n_workers}.csv"
    local log_file="$run_dir/ycsb_w${n_workers}.log"
    touch "$xfm_csv"
    TRANSFORM_PIDS=()

    # ── Start in-memory transform workers ─────────────────────────────────────
    local rounds_per_xfm=$(( FIELD_COUNT * (FIELD_LENGTH / 8) * WORKER_ROUNDS_MULTIPLIER ))
    if (( n_workers > 0 )); then
        log "  Starting $n_workers iBench_cpu worker(s) ..." \
            "(${n_workers} threads × ${TRANSFORMS_PER_WORKER} transforms/s," \
            "rounds=${rounds_per_xfm}," \
            "total=$((n_workers * TRANSFORMS_PER_WORKER)) transforms/s)"
        "$TRANSFORM_BINARY" \
            "$RUNTIME_SECS" "$n_workers" \
            "$TRANSFORMS_PER_WORKER" "$rounds_per_xfm" &
        TRANSFORM_PIDS=($!)
    fi

    # ── Start system monitor ──────────────────────────────────────────────────
    start_monitor "$sys_csv"

    # ── Run YCSB ──────────────────────────────────────────────────────────────
    # -throughput true: runXput() path; binary exits after runtime seconds.
    # -skip: discard the first WARMUP_SKIP_S seconds; report [WARMUP_SKIP_S, RUNTIME_SECS].
    # Prints "throughput mean: X  stddev: Y" at termination.
    log "  Running YCSB: $KNEE_THREADS write threads × ${RUNTIME_SECS}s" \
        "(measuring [${WARMUP_SKIP_S}, ${RUNTIME_SECS}]s window) ..."
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$KNEE_THREADS" \
        -load false -run false -throughput true \
        -runtime "$RUNTIME_SECS" \
        -skip "$WARMUP_SKIP_S" \
        -levels 7 -table baseline \
        2>&1 | tee "$log_file"
    rm -f "$spec"; TMPSPEC=""

    # ── Stop monitor and wait for workers ─────────────────────────────────────
    stop_monitor

    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    TRANSFORM_PIDS=()

    # ── Parse throughput (mean + stddev) from YCSB output ────────────────────
    # Line format: "throughput mean:<mean>  stddev: <stddev>, ..."
    LAST_XPUT_OPS=$(grep -oP 'throughput mean:\s*\K[\d.]+' "$log_file" \
                    2>/dev/null | head -1 || echo "")
    LAST_XPUT_STDDEV=$(grep -oP 'throughput mean:[\d.]+\s+stddev:\s*\K[\d.]+' \
                       "$log_file" 2>/dev/null | head -1 || echo "")

    log "  → ycsb log:     $log_file"
    log "  → system csv:   $sys_csv"
    log "  → transform csv: $xfm_csv"
    log "  → throughput:   ${LAST_XPUT_OPS:-<no reading>} ops/s  (stddev: ${LAST_XPUT_STDDEV:-?})"
}

# ── Main sweep ────────────────────────────────────────────────────────────────
#
# run_sweep:
#   1. Load a fresh DB under OUTPUT_DIR/db.
#   2. Drop the OS page cache.
#   3. Start the per-second system monitor.
#   4. Launch a single YCSB throughput run (RUNTIME_SECS=900, xputwindow=XPUT_WINDOW)
#      in the background.
#   5. Every XPUT_WINDOW seconds add one more in-memory transform worker.
#      Window 0 (t=0..XPUT_WINDOW) is the pure-YCSB baseline with 0 workers.
#   6. Wait for YCSB to finish, then stop the monitor and all workers.
#   7. The per-window CSV written by the binary is the primary output.

run_sweep() {
    sep
    log "CPU slack sweep (single-run incremental mode):"
    log "  Knee threads:    ${KNEE_THREADS}"
    log "  Runtime:         ${RUNTIME_SECS}s  |  window: ${XPUT_WINDOW}s" \
        "→ $((RUNTIME_SECS / XPUT_WINDOW)) windows"
    log "  Record layout:   ${FIELD_COUNT} × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Rate per worker: ${TRANSFORMS_PER_WORKER} integer-mix transforms/s (no I/O)"
    log "  Degrad. thresh:  ${DEGRADATION_THRESHOLD} (plot annotation only — run always completes)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"

    # ── Step 1: create and load fresh DB ──────────────────────────────────────
    local dbpath="$OUTPUT_DIR/db"
    log "  Creating fresh DB at $dbpath"
    load_db "$dbpath" "$sweep_dir/load.log"

    # ── Step 2: drop page cache ────────────────────────────────────────────────
    drop_page_cache

    local xput_file="$sweep_dir/xput_windows.csv"
    local sys_csv="$sweep_dir/system.csv"
    local n_windows=$(( RUNTIME_SECS / XPUT_WINDOW ))

    # ── Step 3: start system monitor ──────────────────────────────────────────
    start_monitor "$sys_csv"

    # ── Step 4: start YCSB in the background ──────────────────────────────────
    log "  Starting YCSB: ${KNEE_THREADS} threads × ${RUNTIME_SECS}s" \
        "(xputwindow=${XPUT_WINDOW}s, skip=0)"
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$KNEE_THREADS" \
        -load false -run false -throughput true \
        -runtime    "$RUNTIME_SECS" \
        -skip       0 \
        -xputwindow "$XPUT_WINDOW" \
        -xputfile   "$xput_file" \
        -levels 7 -table baseline \
        2>&1 | tee "$sweep_dir/ycsb.log" &
    local ycsb_pid=$!
    # Do NOT remove $spec yet — YCSB reads it on startup and we just backgrounded it.
    # It is deleted after wait below.

    # ── Step 5: add one worker per window, starting after window 0 ────────────
    # Window 0 (t=0..XPUT_WINDOW) runs with 0 workers — pure-YCSB baseline.
    # After window 0 ends, launch worker 1; after window 1 ends, launch worker 2; …
    # Each worker is given exactly the remaining time so all workers exit cleanly.
    local sweep_start=$SECONDS
    TRANSFORM_PIDS=()
    sleep "$XPUT_WINDOW"   # let window 0 complete with no workers

    local w
    for (( w = 1; w < n_windows; w++ )); do
        # Check that YCSB is still running before launching the next worker.
        if ! kill -0 "$ycsb_pid" 2>/dev/null; then
            log "  YCSB exited early — stopping worker loop at window ${w}"
            break
        fi

        local elapsed=$(( SECONDS - sweep_start ))
        local remaining=$(( RUNTIME_SECS - elapsed ))
        (( remaining <= 0 )) && break

        local rounds_per_xfm=$(( FIELD_COUNT * (FIELD_LENGTH / 8) * WORKER_ROUNDS_MULTIPLIER ))
        log "  Window ${w}: launching iBench_cpu worker ${w}" \
            "(~${remaining}s remaining," \
            "total ~$((w * TRANSFORMS_PER_WORKER)) transforms/s," \
            "rounds=${rounds_per_xfm})"

        # Rate=0 → saturate mode (busy-loop): each worker pegs one core at 100%.
        # The sweep varies the *number* of fully-loaded workers, not per-worker
        # intensity.  Passing TRANSFORMS_PER_WORKER as the rate limit made each
        # worker sleep ~98% of the time (effectively invisible on CPU metrics).
        "$TRANSFORM_BINARY" \
            "$remaining" 1 0 "$rounds_per_xfm" &
        TRANSFORM_PIDS+=($!)

        sleep "$XPUT_WINDOW"
    done

    # ── Step 6: wait for YCSB, then clean up ──────────────────────────────────
    wait "$ycsb_pid" 2>/dev/null || true
    rm -f "$spec"; TMPSPEC=""   # safe to remove now that YCSB has exited
    stop_monitor

    local pid
    for pid in "${TRANSFORM_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    TRANSFORM_PIDS=()

    log "  → ycsb log:      $sweep_dir/ycsb.log"
    log "  → xput windows:  $xput_file"
    log "  → system csv:    $sys_csv"
    log "Sweep complete."
}

# ── Python plotter ────────────────────────────────────────────────────────────

write_plotter() {
    PLOTTER_SCRIPT=$(mktemp /tmp/slack_plotter_XXXXX.py)
    cat > "$PLOTTER_SCRIPT" << 'PYPLOT'
#!/usr/bin/env python3
"""
cpu_slack_plotter.py  (single-run incremental-worker edition)

Reads:
  sweep/xput_windows.csv   — per-window throughput written by the YCSB binary
                             columns: window_start_sec, avg_throughput, stddev_throughput
  sweep/system.csv         — per-second CPU/disk monitor output

Produces:
  plots/cpu_slack_curve.png   — 3-panel plot: throughput, CPU, disk vs time
                                 with per-window worker-count annotations
  plots/cpu_slack_summary.csv — machine-readable per-window summary

The X-axis is time (window_start_sec).  A secondary tick label shows the number
of transform workers active during each window (0 in window 0, +1 each window).
A red dashed baseline marks KNEE_XPUT (if provided).
A vertical dashed line marks the last window where throughput stays within
DEGRADATION_PCT of the window-0 baseline.
"""
import sys, os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

OUTPUT_DIR      = sys.argv[1]
XPUT_WINDOW     = int(sys.argv[2])    # seconds per window
KNEE_XPUT       = float(sys.argv[3])  # 0 = use window-0 as baseline
RATE_PER_WORKER = int(sys.argv[4])
FIELD_COUNT     = int(sys.argv[5])
FIELD_LENGTH    = int(sys.argv[6])
DEGRADATION_PCT = float(sys.argv[7])  # e.g. 0.10

SWEEP_DIR = os.path.join(OUTPUT_DIR, "sweep")
PLOT_DIR  = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

# ── Load xput_windows.csv ────────────────────────────────────────────────────

xput_path = os.path.join(SWEEP_DIR, "xput_windows.csv")
try:
    xw = pd.read_csv(xput_path)
    # Expected columns: window_start_sec, avg_throughput, stddev_throughput
    if 'window_start_sec' not in xw.columns:
        # Fallback: treat first col as window_start_sec
        xw.columns = ['window_start_sec', 'avg_throughput', 'stddev_throughput']
except Exception as e:
    print(f"ERROR: could not read {xput_path}: {e}")
    sys.exit(1)

# Derive worker count for each window: window index = row position.
# Window 0 → 0 workers; window k → k workers.
xw = xw.sort_values('window_start_sec').reset_index(drop=True)
xw['workers'] = xw.index.astype(int)   # 0, 1, 2, …
xw['total_rate_target'] = xw['workers'] * RATE_PER_WORKER

# ── Load system.csv and aggregate per window ─────────────────────────────────

sys_path = os.path.join(SWEEP_DIR, "system.csv")
sys_rows = []
try:
    sys_df = pd.read_csv(sys_path)
    sys_df['elapsed_s'] = sys_df['timestamp_s'] - sys_df['timestamp_s'].iloc[0]
    iowait_col = sys_df['cpu_iowait_pct'] if 'cpu_iowait_pct' in sys_df.columns \
                 else pd.Series(0.0, index=sys_df.index)
    sys_df['cpu_busy']    = 100.0 - sys_df['cpu_idle_pct']
    sys_df['cpu_compute'] = sys_df['cpu_busy'] - iowait_col

    ctx_col = sys_df['ctx_switches_per_sec'] if 'ctx_switches_per_sec' in sys_df.columns \
              else pd.Series(np.nan, index=sys_df.index)

    for _, wrow in xw.iterrows():
        t0 = wrow['window_start_sec']
        t1 = t0 + XPUT_WINDOW
        win = sys_df[(sys_df['elapsed_s'] >= t0) & (sys_df['elapsed_s'] < t1)]
        if win.empty:
            sys_rows.append(dict(cpu_compute_mean=np.nan, cpu_busy_mean=np.nan,
                                 cpu_iowait_mean=np.nan,
                                 cpu_compute_std=np.nan,  cpu_busy_std=np.nan,
                                 disk_read_mean=np.nan,   disk_read_std=np.nan,
                                 disk_write_mean=np.nan,  disk_write_std=np.nan,
                                 ctx_switches_mean=np.nan))
        else:
            sys_rows.append(dict(
                cpu_compute_mean  = win['cpu_compute'].mean(),
                cpu_busy_mean     = win['cpu_busy'].mean(),
                cpu_iowait_mean   = iowait_col[win.index].mean(),
                cpu_compute_std   = win['cpu_compute'].std(ddof=1),
                cpu_busy_std      = win['cpu_busy'].std(ddof=1),
                disk_read_mean    = win['disk_read_mbs'].mean(),
                disk_read_std     = win['disk_read_mbs'].std(ddof=1),
                disk_write_mean   = win['disk_write_mbs'].mean(),
                disk_write_std    = win['disk_write_mbs'].std(ddof=1),
                ctx_switches_mean = ctx_col[win.index].mean(),
            ))
except Exception as e:
    print(f"  [warn] Could not parse {sys_path}: {e}")
    sys_rows = [dict(cpu_compute_mean=np.nan, cpu_busy_mean=np.nan,
                     cpu_iowait_mean=np.nan,  cpu_active_std=np.nan,
                     disk_read_mean=np.nan,   disk_write_mean=np.nan)] * len(xw)

sys_agg = pd.DataFrame(sys_rows)
df = pd.concat([xw.reset_index(drop=True), sys_agg.reset_index(drop=True)], axis=1)

df.to_csv(os.path.join(PLOT_DIR, "cpu_slack_summary.csv"), index=False)
print("Per-window summary:")
print(df[['window_start_sec','workers','total_rate_target',
          'avg_throughput','stddev_throughput',
          'cpu_compute_mean','cpu_busy_mean',
          'disk_write_mean','disk_read_mean']].to_string(index=False))

# ── Detect slack boundary ─────────────────────────────────────────────────────
# Last window where throughput stays within DEGRADATION_PCT of the window-0 baseline.

baseline_xput = KNEE_XPUT if KNEE_XPUT > 0 else df['avg_throughput'].dropna().iloc[0]
slack_boundary_w = None   # worker count at last good window
valid = df.dropna(subset=['avg_throughput'])
for _, row in valid.iterrows():
    if baseline_xput > 0 and (baseline_xput - row['avg_throughput']) / baseline_xput <= DEGRADATION_PCT:
        slack_boundary_w = int(row['workers'])
    else:
        if slack_boundary_w is not None:
            break   # stop at first degraded window after at least one good window

if slack_boundary_w is not None:
    print(f"Slack boundary: {slack_boundary_w} workers "
          f"× {RATE_PER_WORKER} transforms/worker/s"
          f"  (= {slack_boundary_w * RATE_PER_WORKER} transforms/s total)")

# ── Plot ──────────────────────────────────────────────────────────────────────
# X-axis: time (window_start_sec).
# Each data point is one XPUT_WINDOW-second bucket.
# Top tick labels: elapsed time (s).
# Bottom tick labels: worker count active during that window.

plt.rcParams.update({
    'figure.dpi': 150, 'font.size': 11,
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.grid': True, 'grid.alpha': 0.35,
    'axes.labelsize': 11, 'xtick.labelsize': 9, 'ytick.labelsize': 10,
})

fig = plt.figure(figsize=(14, 13))
gs  = gridspec.GridSpec(3, 1, hspace=0.55)
ax_xput = fig.add_subplot(gs[0])
ax_cpu  = fig.add_subplot(gs[1], sharex=ax_xput)
ax_disk = fig.add_subplot(gs[2], sharex=ax_xput)

t    = df['window_start_sec'].values
wkrs = df['workers'].values

def add_slack_vline(ax, label=True):
    if slack_boundary_w is not None:
        deg_rows = df[df['workers'] == slack_boundary_w + 1]
        if not deg_rows.empty:
            vx = deg_rows.iloc[0]['window_start_sec']
            lbl = f'Slack boundary: {slack_boundary_w} workers' if label else None
            ax.axvline(vx, color='purple', linestyle=':', alpha=0.75, label=lbl)

# ── Panel 1: write throughput with min/max band ──────────────────────────────

# Shaded min/max band behind the mean line shows the full per-second range
# within each window — a single write stall tanks the min even when the mean
# and stddev error bars look modest.
if 'min_throughput' in df.columns and 'max_throughput' in df.columns:
    ax_xput.fill_between(t, df['min_throughput'], df['max_throughput'],
                         alpha=0.18, color='steelblue', label='Min/max range')
ax_xput.errorbar(t, df['avg_throughput'], yerr=df['stddev_throughput'],
                 fmt='o-', color='steelblue', linewidth=2, markersize=6,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Mean throughput  ± 1σ')
baseline_label = 'Knee baseline' if KNEE_XPUT > 0 else 'Window-0 baseline'
ax_xput.axhline(baseline_xput, color='crimson', linestyle='--', alpha=0.7,
                label=f'{baseline_label} ({baseline_xput:.0f} ops/s)')
ax_xput.axhline(baseline_xput * (1 - DEGRADATION_PCT),
                color='crimson', linestyle=':', alpha=0.45,
                label=f'−{DEGRADATION_PCT*100:.0f}% threshold')
add_slack_vline(ax_xput)
ax_xput.set_ylabel('Write throughput\n(ops / sec)')
ax_xput.set_title('Write throughput vs concurrent transform workers  (± 1σ, shaded = min/max)')
ax_xput.legend(fontsize=9, loc='lower left')

# ── Panel 2: CPU utilization with error bars ─────────────────────────────────

ax_cpu.errorbar(t, df['cpu_busy_mean'], yerr=df['cpu_busy_std'],
                fmt='s--', color='steelblue', linewidth=2, markersize=6,
                capsize=4, capthick=1.5, elinewidth=1.5,
                label='cpu_busy = 100−idle  (incl. iowait) %')
ax_cpu.errorbar(t, df['cpu_compute_mean'], yerr=df['cpu_compute_std'],
                fmt='^-', color='forestgreen', linewidth=2, markersize=6,
                capsize=4, capthick=1.5, elinewidth=1.5,
                label='cpu_compute = 100−idle−iowait  (pure compute) %')
ax_cpu.fill_between(t, df['cpu_compute_mean'], df['cpu_busy_mean'],
                    alpha=0.13, color='steelblue', label='iowait gap')
ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.55, label='100% ceiling')
add_slack_vline(ax_cpu, label=False)
ax_cpu.set_ylabel('CPU utilization (%)')
ax_cpu.set_title('System CPU utilization  (± 1σ)  —  gap = iowait')
ax_cpu.legend(fontsize=9, loc='lower right')

# ── Panel 3: disk bandwidth with error bars ───────────────────────────────────

ax_disk.errorbar(t, df['disk_write_mean'], yerr=df['disk_write_std'],
                 fmt='s-', color='darkorange', linewidth=2, markersize=6,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Disk write (MB/s)  ± 1σ')
ax_disk.errorbar(t, df['disk_read_mean'], yerr=df['disk_read_std'],
                 fmt='D--', color='saddlebrown', linewidth=2, markersize=5,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Disk read (MB/s)  ± 1σ')
add_slack_vline(ax_disk, label=False)
ax_disk.set_ylabel('Disk I/O\n(MB / sec)')
ax_disk.set_title('Disk bandwidth  (± 1σ)')
ax_disk.legend(fontsize=9, loc='lower left')

# ── X-axis: concurrent worker count only ─────────────────────────────────────

tick_labels = [f'w={int(w)}' for w in wkrs]
ax_disk.set_xticks(t)
ax_disk.set_xticklabels(tick_labels, rotation=45, ha='right', fontsize=8)
ax_disk.set_xlabel('Concurrent transform workers  (w=0 = baseline)', fontsize=10)

plt.setp(ax_xput.get_xticklabels(), visible=False)
plt.setp(ax_cpu.get_xticklabels(),  visible=False)

record_bytes = FIELD_COUNT * FIELD_LENGTH
slack_str = (f"  |  slack ≤ {slack_boundary_w} workers × {RATE_PER_WORKER}/s"
             if slack_boundary_w is not None else "")
fig.suptitle(
    f'RocksDB CPU slack — incremental worker sweep  '
    f'({FIELD_COUNT}×{FIELD_LENGTH}B={record_bytes}B/rec, '
    f'{RATE_PER_WORKER} xfm/worker/s){slack_str}',
    fontsize=12, fontweight='bold', y=0.998)

out = os.path.join(PLOT_DIR, "cpu_slack_curve.png")
plt.savefig(out, bbox_inches='tight')
plt.close()
print(f"\nPlot saved: {out}")
PYPLOT
}

# ── Combined saturation + slack plotter ───────────────────────────────────────
#
# When SATURATION_SUMMARY is set, this plotter reads both the saturation-phase
# summary.csv (per-thread-count CPU/disk aggregates) and the slack-sweep
# system.csv (per-second data aggregated per window).  The X-axis is unified:
#   1, 2, 4, 8, 12, 16, 16+1, 16+2, ..., 16+28
# where KNEE_THREADS=16 in this example.

write_combined_plotter() {
    COMBINED_PLOTTER_SCRIPT=$(mktemp /tmp/slack_combined_plotter_XXXXX.py)
    cat > "$COMBINED_PLOTTER_SCRIPT" << 'PYCOMB'
#!/usr/bin/env python3
"""
combined_saturation_slack_plotter.py

Reads:
  1. Saturation-phase summary.csv — per-thread-count aggregated metrics.
  2. Slack-sweep system.csv + xput_windows.csv — per-window metrics.

Produces a 3-panel plot with unified X-axis:
  Panel 1: Write throughput
  Panel 2: CPU utilization (compute + busy + iowait gap)
  Panel 3: Disk bandwidth (read + write)

X-axis ticks:  1, 2, 4, …, K, K+1, K+2, …, K+N
where K = KNEE_THREADS and N = number of slack-sweep workers.
A vertical dashed line separates the saturation phase from the slack phase.
"""
import sys, os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from matplotlib.ticker import FixedLocator, FixedFormatter

OUTPUT_DIR        = sys.argv[1]
SATURATION_CSV    = sys.argv[2]    # summary.csv from saturation_sweep.sh
XPUT_WINDOW       = int(sys.argv[3])
KNEE_THREADS      = int(sys.argv[4])
KNEE_XPUT         = float(sys.argv[5])
DEGRADATION_PCT   = float(sys.argv[6])

SWEEP_DIR = os.path.join(OUTPUT_DIR, "sweep")
PLOT_DIR  = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

# ── Load saturation summary ───────────────────────────────────────────────────

sat_df = pd.read_csv(SATURATION_CSV)
sat_df = sat_df.sort_values('threads').reset_index(drop=True)

# ── Load slack-sweep xput_windows.csv ─────────────────────────────────────────

xput_path = os.path.join(SWEEP_DIR, "xput_windows.csv")
try:
    xw = pd.read_csv(xput_path)
    if 'window_start_sec' not in xw.columns:
        xw.columns = ['window_start_sec', 'avg_throughput', 'stddev_throughput']
    xw = xw.sort_values('window_start_sec').reset_index(drop=True)
    xw['workers'] = xw.index.astype(int)
except Exception as e:
    print(f"ERROR: could not read {xput_path}: {e}")
    sys.exit(1)

# ── Load slack-sweep system.csv and aggregate per window ──────────────────────

sys_path = os.path.join(SWEEP_DIR, "system.csv")
try:
    sys_df = pd.read_csv(sys_path)
    sys_df['elapsed_s'] = sys_df['timestamp_s'] - sys_df['timestamp_s'].iloc[0]
    iowait_col = sys_df['cpu_iowait_pct'] if 'cpu_iowait_pct' in sys_df.columns \
                 else pd.Series(0.0, index=sys_df.index)
    sys_df['cpu_busy']    = 100.0 - sys_df['cpu_idle_pct']
    sys_df['cpu_compute'] = sys_df['cpu_busy'] - iowait_col
except Exception as e:
    print(f"ERROR: could not read {sys_path}: {e}")
    sys_df = pd.DataFrame()

slack_sys_rows = []
for _, wrow in xw.iterrows():
    t0 = wrow['window_start_sec']
    t1 = t0 + XPUT_WINDOW
    if sys_df.empty:
        slack_sys_rows.append(dict(
            cpu_compute_mean=np.nan, cpu_busy_mean=np.nan, cpu_iowait_mean=np.nan,
            cpu_compute_std=np.nan,  cpu_busy_std=np.nan,
            disk_read_mean=np.nan,   disk_read_std=np.nan,
            disk_write_mean=np.nan,  disk_write_std=np.nan))
        continue
    win = sys_df[(sys_df['elapsed_s'] >= t0) & (sys_df['elapsed_s'] < t1)]
    if win.empty:
        slack_sys_rows.append(dict(
            cpu_compute_mean=np.nan, cpu_busy_mean=np.nan, cpu_iowait_mean=np.nan,
            cpu_compute_std=np.nan,  cpu_busy_std=np.nan,
            disk_read_mean=np.nan,   disk_read_std=np.nan,
            disk_write_mean=np.nan,  disk_write_std=np.nan))
    else:
        iw = iowait_col[win.index] if not sys_df.empty else pd.Series(0.0)
        slack_sys_rows.append(dict(
            cpu_compute_mean = win['cpu_compute'].mean(),
            cpu_busy_mean    = win['cpu_busy'].mean(),
            cpu_iowait_mean  = iw.mean(),
            cpu_compute_std  = win['cpu_compute'].std(ddof=1),
            cpu_busy_std     = win['cpu_busy'].std(ddof=1),
            disk_read_mean   = win['disk_read_mbs'].mean(),
            disk_read_std    = win['disk_read_mbs'].std(ddof=1),
            disk_write_mean  = win['disk_write_mbs'].mean(),
            disk_write_std   = win['disk_write_mbs'].std(ddof=1),
        ))

slack_agg = pd.DataFrame(slack_sys_rows)
slack_df  = pd.concat([xw.reset_index(drop=True), slack_agg.reset_index(drop=True)], axis=1)

# ── Build unified X-axis ──────────────────────────────────────────────────────
# Saturation phase: x-position = thread count (1, 2, 4, 8, …, K)
# Slack phase:      x-position = K + worker_count (K+1, K+2, …, K+N)

sat_x = sat_df['threads'].values.astype(float)
slack_x = KNEE_THREADS + slack_df['workers'].values.astype(float)

# Labels for saturation: just the thread count
sat_labels = [str(int(t)) for t in sat_x]
# Labels for slack: K+w format
slack_labels = [f"{KNEE_THREADS}+{int(w)}" for w in slack_df['workers'].values]

all_x      = np.concatenate([sat_x, slack_x])
all_labels = sat_labels + slack_labels

# ── Detect baseline and slack boundary ────────────────────────────────────────
baseline_xput = KNEE_XPUT if KNEE_XPUT > 0 else (
    slack_df['avg_throughput'].dropna().iloc[0] if not slack_df['avg_throughput'].dropna().empty
    else np.nan)

slack_boundary_w = None
for _, row in slack_df.dropna(subset=['avg_throughput']).iterrows():
    if baseline_xput > 0 and (baseline_xput - row['avg_throughput']) / baseline_xput <= DEGRADATION_PCT:
        slack_boundary_w = int(row['workers'])
    else:
        if slack_boundary_w is not None:
            break

# ── Plot ──────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    'figure.dpi': 150, 'font.size': 11,
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.grid': True, 'grid.alpha': 0.35,
    'axes.labelsize': 11, 'xtick.labelsize': 8, 'ytick.labelsize': 10,
})

fig = plt.figure(figsize=(16, 14))
gs  = gridspec.GridSpec(3, 1, hspace=0.50)
ax_xput = fig.add_subplot(gs[0])
ax_cpu  = fig.add_subplot(gs[1], sharex=ax_xput)
ax_disk = fig.add_subplot(gs[2], sharex=ax_xput)

# Vertical separator at the knee
for ax in (ax_xput, ax_cpu, ax_disk):
    ax.axvline(KNEE_THREADS + 0.5, color='gray', linestyle='--', alpha=0.6, linewidth=1.5)

def add_slack_boundary_vline(ax, label=True):
    if slack_boundary_w is not None:
        vx = KNEE_THREADS + slack_boundary_w + 0.5
        lbl = f'Slack boundary: {slack_boundary_w} workers' if label else None
        ax.axvline(vx, color='purple', linestyle=':', alpha=0.75, label=lbl)

# ── Panel 1: Write throughput ─────────────────────────────────────────────────
# Saturation phase
if 'xput_std' in sat_df.columns:
    ax_xput.errorbar(sat_x, sat_df['xput_mean'], yerr=sat_df['xput_std'],
                     fmt='o-', color='steelblue', linewidth=2, markersize=6,
                     capsize=4, capthick=1.5, elinewidth=1.5,
                     label='Saturation phase (throughput ± 1σ)')
else:
    ax_xput.plot(sat_x, sat_df['xput_mean'], 'o-', color='steelblue',
                 linewidth=2, markersize=6, label='Saturation phase')

# Slack phase
ax_xput.errorbar(slack_x, slack_df['avg_throughput'], yerr=slack_df['stddev_throughput'],
                 fmt='s-', color='darkorange', linewidth=2, markersize=6,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Slack phase (throughput ± 1σ)')

if not np.isnan(baseline_xput):
    ax_xput.axhline(baseline_xput, color='crimson', linestyle='--', alpha=0.7,
                    label=f'Knee baseline ({baseline_xput:.0f} ops/s)')
    ax_xput.axhline(baseline_xput * (1 - DEGRADATION_PCT),
                    color='crimson', linestyle=':', alpha=0.45,
                    label=f'−{DEGRADATION_PCT*100:.0f}% threshold')
add_slack_boundary_vline(ax_xput)
ax_xput.set_ylabel('Write throughput\n(ops / sec)')
ax_xput.set_title('Write throughput — saturation sweep → slack sweep', fontsize=12)
ax_xput.legend(fontsize=8, loc='best')

# ── Panel 2: CPU utilization ──────────────────────────────────────────────────
# Saturation phase
if 'cpu_busy_mean' in sat_df.columns:
    sat_cpu_busy = sat_df['cpu_busy_mean']
    sat_cpu_compute = sat_df.get('cpu_compute_mean', sat_cpu_busy)
    sat_cpu_std = sat_df.get('cpu_active_std', pd.Series(0.0, index=sat_df.index))

    ax_cpu.errorbar(sat_x, sat_cpu_busy, yerr=sat_cpu_std,
                    fmt='s--', color='steelblue', linewidth=2, markersize=6,
                    capsize=3, capthick=1.2, elinewidth=1.2,
                    label='Saturation: cpu_busy (100−idle) %')
    ax_cpu.errorbar(sat_x, sat_cpu_compute,
                    fmt='^-', color='forestgreen', linewidth=2, markersize=6,
                    capsize=3, capthick=1.2, elinewidth=1.2,
                    label='Saturation: cpu_compute (100−idle−iowait) %')
    ax_cpu.fill_between(sat_x, sat_cpu_compute, sat_cpu_busy,
                        alpha=0.10, color='steelblue')

# Slack phase
ax_cpu.errorbar(slack_x, slack_df['cpu_busy_mean'], yerr=slack_df.get('cpu_busy_std', 0),
                fmt='s--', color='darkorange', linewidth=2, markersize=6,
                capsize=3, capthick=1.2, elinewidth=1.2,
                label='Slack: cpu_busy %')
ax_cpu.errorbar(slack_x, slack_df['cpu_compute_mean'], yerr=slack_df.get('cpu_compute_std', 0),
                fmt='^-', color='darkgreen', linewidth=2, markersize=6,
                capsize=3, capthick=1.2, elinewidth=1.2,
                label='Slack: cpu_compute %')
ax_cpu.fill_between(slack_x, slack_df['cpu_compute_mean'], slack_df['cpu_busy_mean'],
                    alpha=0.10, color='darkorange')

ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.55, label='100% ceiling')
add_slack_boundary_vline(ax_cpu, label=False)
ax_cpu.set_ylabel('CPU utilization (%)')
ax_cpu.set_title('System CPU utilization — gap = iowait', fontsize=12)
ax_cpu.legend(fontsize=7, loc='best', ncol=2)

# ── Panel 3: Disk bandwidth ──────────────────────────────────────────────────
# Saturation phase
if 'disk_read_mb/s' in sat_df.columns:
    ax_disk.errorbar(sat_x, sat_df['disk_write_mb/s'],
                     yerr=sat_df.get('disk_write_std', 0),
                     fmt='s-', color='steelblue', linewidth=2, markersize=6,
                     capsize=3, capthick=1.2, elinewidth=1.2,
                     label='Saturation: disk write (MB/s)')
    ax_disk.errorbar(sat_x, sat_df['disk_read_mb/s'],
                     yerr=sat_df.get('disk_read_std', 0),
                     fmt='D--', color='cornflowerblue', linewidth=2, markersize=5,
                     capsize=3, capthick=1.2, elinewidth=1.2,
                     label='Saturation: disk read (MB/s)')

# Slack phase
ax_disk.errorbar(slack_x, slack_df['disk_write_mean'],
                 yerr=slack_df.get('disk_write_std', 0),
                 fmt='s-', color='darkorange', linewidth=2, markersize=6,
                 capsize=3, capthick=1.2, elinewidth=1.2,
                 label='Slack: disk write (MB/s)')
ax_disk.errorbar(slack_x, slack_df['disk_read_mean'],
                 yerr=slack_df.get('disk_read_std', 0),
                 fmt='D--', color='saddlebrown', linewidth=2, markersize=5,
                 capsize=3, capthick=1.2, elinewidth=1.2,
                 label='Slack: disk read (MB/s)')
add_slack_boundary_vline(ax_disk, label=False)
ax_disk.set_ylabel('Disk I/O\n(MB / sec)')
ax_disk.set_title('Disk bandwidth', fontsize=12)
ax_disk.legend(fontsize=8, loc='best')

# ── X-axis ticks ──────────────────────────────────────────────────────────────

ax_disk.set_xticks(all_x)
ax_disk.set_xticklabels(all_labels, rotation=45, ha='right', fontsize=7)
ax_disk.set_xlabel(
    f'← Saturation (client threads) │ Slack sweep (workers at {KNEE_THREADS} threads) →',
    fontsize=10)

plt.setp(ax_xput.get_xticklabels(), visible=False)
plt.setp(ax_cpu.get_xticklabels(),  visible=False)

# Add phase labels
fig.text(0.25, 0.01, 'Saturation Phase', ha='center', fontsize=10, fontstyle='italic', color='steelblue')
fig.text(0.70, 0.01, 'Slack Sweep Phase', ha='center', fontsize=10, fontstyle='italic', color='darkorange')

slack_str = (f"  |  slack ≤ {slack_boundary_w} workers"
             if slack_boundary_w is not None else "")
fig.suptitle(
    f'RocksDB characterization — saturation + CPU slack sweep'
    f'{slack_str}',
    fontsize=13, fontweight='bold', y=0.998)

out = os.path.join(PLOT_DIR, "combined_saturation_slack.png")
plt.savefig(out, bbox_inches='tight')
plt.close()
print(f"\nCombined plot saved: {out}")
PYCOMB
}

run_plotter() {
    sep
    log "Generating CPU slack plots..."
    write_plotter
    # Pass XPUT_WINDOW so the plotter can align system-CSV windows with the
    # per-window throughput CSV rows written by the YCSB binary.
    python3 "$PLOTTER_SCRIPT" \
        "$OUTPUT_DIR" "$XPUT_WINDOW" "$KNEE_XPUT" \
        "$TRANSFORMS_PER_WORKER" "$FIELD_COUNT" "$FIELD_LENGTH" \
        "$DEGRADATION_THRESHOLD"

    # ── Combined saturation + slack plot (when saturation data is available) ──
    if [[ -n "$SATURATION_SUMMARY" && -f "$SATURATION_SUMMARY" ]]; then
        log "Generating combined saturation + slack plot..."
        write_combined_plotter
        python3 "$COMBINED_PLOTTER_SCRIPT" \
            "$OUTPUT_DIR" "$SATURATION_SUMMARY" \
            "$XPUT_WINDOW" "$KNEE_THREADS" "$KNEE_XPUT" \
            "$DEGRADATION_THRESHOLD"
    else
        log "  (Skipping combined plot — SATURATION_SUMMARY not set or not found)"
        log "  Set SATURATION_SUMMARY=/path/to/summary.csv to enable."
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    log "cpu_slack_sweep.sh starting  (incremental-worker single-run mode)"
    log "  Binary:          $BINARY"
    log "  Output dir:      $OUTPUT_DIR"
    log "  Device:          $DISK_DEVICE"
    log "  Knee threads:    $KNEE_THREADS  (RocksDB write threads)"
    log "  Knee xput:       ${KNEE_XPUT} ops/s  (plot annotation; 0 = use window-0 as baseline)"
    log "  DB path:         $OUTPUT_DIR/db  (created fresh)"
    log "  Record count:    ${RECORD_COUNT}"
    log "  Value layout:    ${FIELD_COUNT} fields × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Transform rate:  ${TRANSFORMS_PER_WORKER} transforms/worker/s  (in-memory integer mix)"
    log "  Runtime:         ${RUNTIME_SECS}s  ÷  window ${XPUT_WINDOW}s  = $((RUNTIME_SECS / XPUT_WINDOW)) windows"
    log "  Worker schedule: +1 worker per window (window 0 = baseline, no workers)"
    log "  Degrad. thresh:  ${DEGRADATION_THRESHOLD}  (plot annotation only)"
    log "  Sat. summary:    ${SATURATION_SUMMARY:-<not set>}"
    echo ""

    check_prereqs
    setup_monitor_script
    compile_transform_binary
    mkdir -p "$OUTPUT_DIR"

    run_sweep
    run_plotter

    sep
    log "Done."
    log "  Results:        $OUTPUT_DIR/sweep/"
    log "  Plot:           $OUTPUT_DIR/plots/cpu_slack_curve.png"
    log "  Summary CSV:    $OUTPUT_DIR/plots/cpu_slack_summary.csv"
    log "  Per-window CSV: $OUTPUT_DIR/sweep/xput_windows.csv"
    log ""
    log "  Minimal invocation:"
    log "    KNEE_THREADS=16 \\"
    log "    WORKLOAD_SPEC=/path/to/workload.spec \\"
    log "    bash ../utility/cpu_slack_sweep.sh"
    log ""
    log "  Serve locally:  python3 -m http.server 8888 --directory $OUTPUT_DIR"
}

main "$@"
