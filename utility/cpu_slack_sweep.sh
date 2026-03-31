#!/usr/bin/env bash
# =============================================================================
# cpu_slack_sweep.sh — Phase 2: CPU slack characterization
#
# Story: at the knee operating point found by saturation_sweep.sh, run YCSB
# writers at the knee thread count while concurrently increasing the load from
# rate-limited transform worker threads.  Each worker simulates one Mycelium
# mRoutine pipeline: it performs field_count × (field_length / 8) rounds of
# pure integer arithmetic (Murmur3-style mix) per transform, sleeping between
# transforms to hit a fixed rate target.  No buffers, no memory or disk I/O.  The sweep is over the number of
# concurrent worker threads.
#
# Because transforms in production fire once per compacted record, the per-
# worker rate is set to TRANSFORMS_PER_WORKER_PER_SEC (default 1000/s).
# Total transform load at step k = k × TRANSFORMS_PER_WORKER_PER_SEC.
# The claim produced: "baseline absorbs up to K transform workers
# (= K × R transforms/s) before write throughput degrades."
#
# Run from the project BUILD directory:
#   cd /path/to/build && 
#     DB_PATH=<sat_results_dir>/sweep/rocksdb bash ../utility/cpu_slack_sweep.sh
#
# Minimum required configuration (set these from Phase 1 results):
#   KNEE_THREADS=<N>        client write threads at the knee
#   KNEE_XPUT=<ops/s>       knee throughput (for baseline annotation)
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   run_w<K>/
#     ycsb_w<K>.log          — YCSB output (throughput mean+stddev inside)
#     system_w<K>.csv        — per-second CPU/disk samples
#     transform_w<K>.csv     — per-second transform counts per worker thread
#   plots/
#     cpu_slack_curve.png    — 4-panel plot on same X-axis (transform workers)
#     cpu_slack_summary.csv  — machine-readable summary
# =============================================================================

set -euo pipefail

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

# ── Phase 1 results — set these (or source phase2_config.env) ─────────────────

# Path to the RocksDB directory written by saturation_sweep.sh Phase 1.
# Typically: <sat_results_dir>/sweep/rocksdb
# The DB is reused as-is; no loading step is performed here.
DB_PATH="${DB_PATH:-}"

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

DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Total seconds each run_one() YCSB instance runs.
# The binary discards the first WARMUP_SKIP_S seconds (passed via -skip) and
# reports the mean/stddev over [WARMUP_SKIP_S, RUNTIME_SECS].  Must be strictly
# greater than WARMUP_SKIP_S.  Matching saturation_sweep.sh defaults gives a
# 90 s steady-state window: [90, 180].
RUNTIME_SECS="${RUNTIME_SECS:-180}"

# Warmup seconds to discard.  Passed to the binary via -skip and used by the
# plotter to trim the system-monitor CSV to the same steady-state window.
# Must match the value used in Phase 1 (saturation_sweep.sh) so that KNEE_XPUT
# was measured over the same window length as Phase 2 steps.
WARMUP_SKIP_S="${WARMUP_SKIP_S:-90}"

# Throughput degradation threshold: stop the sweep when YCSB client throughput
# drops more than this fraction below the 0-worker baseline.
DEGRADATION_THRESHOLD="${DEGRADATION_THRESHOLD:-0.10}"

# Number of consecutive above-threshold drops required before the sweep stops.
# A single compaction event can spike throughput down by 10-20 % for one step;
# requiring N_CONSECUTIVE consecutive above-threshold steps filters those
# one-off dips out.  Set to 1 to restore the original single-step behaviour.
N_CONSECUTIVE="${N_CONSECUTIVE:-2}"

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
TRANSFORM_SCRIPT=""
PLOTTER_SCRIPT=""
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
    [[ -n "${MONITOR_SCRIPT:-}"   ]] && rm -f "$MONITOR_SCRIPT"
    [[ -n "${TRANSFORM_SCRIPT:-}" ]] && rm -f "$TRANSFORM_SCRIPT"
    [[ -n "${PLOTTER_SCRIPT:-}"   ]] && rm -f "$PLOTTER_SCRIPT"
    [[ -n "${TMPSPEC:-}"          ]] && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]] || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC is not set.  Point it at a .spec file."
    [[ -f "$WORKLOAD_SPEC" ]]  || die "WORKLOAD_SPEC file not found: $WORKLOAD_SPEC"
    [[ -n "$DB_PATH" ]]        || die "DB_PATH is not set.  Point it at the Phase 1 RocksDB directory (e.g. <sat_results_dir>/sweep/rocksdb)."
    [[ -d "$DB_PATH" ]]        || die "DB_PATH directory not found: $DB_PATH"
    python3 -c "v=float('${KNEE_XPUT}'); exit(0 if v > 0 else 1)" 2>/dev/null \
        || die "KNEE_XPUT must be a positive number (got '${KNEE_XPUT}').  Source phase2_config.env or set it manually."
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    python3 -c "import matplotlib, pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user matplotlib pandas numpy -q \
            || sudo apt-get install -y python3-matplotlib python3-pandas python3-numpy -q
    }
    [[ -d "$WORKLOAD_DIR" ]] || die "Workload dir not found: $WORKLOAD_DIR"
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: device '$DISK_DEVICE' not found — disk I/O will be zero."
        log "  Available: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
    fi
    (( RUNTIME_SECS > WARMUP_SKIP_S )) || \
        die "RUNTIME_SECS ($RUNTIME_SECS) must be > WARMUP_SKIP_S ($WARMUP_SKIP_S)"
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
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]

def read_disk(dev):
    if not dev:
        return 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                return int(p[5]), int(p[9])
    return 0, 0

prev_cpu = read_cpu()
prev_rd, prev_wr = read_disk(disk_device)
prev_t   = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s', 'cpu_user_pct', 'cpu_sys_pct',
                'cpu_iowait_pct', 'cpu_idle_pct',
                'disk_read_mbs', 'disk_write_mbs'])
    fh.flush()
    while True:
        time.sleep(1)
        now     = time.time()
        cur_cpu = read_cpu()
        cur_rd, cur_wr = read_disk(disk_device)

        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        idle_pct   = 100.0 * delta[3] / total
        user_pct   = 100.0 * delta[0] / total
        sys_pct    = 100.0 * delta[2] / total
        iowait_pct = 100.0 * delta[4] / total

        dt = (now - prev_t) or 1
        rd_mbs = (cur_rd - prev_rd) * 512 / 1024 / 1024 / dt
        wr_mbs = (cur_wr - prev_wr) * 512 / 1024 / 1024 / dt

        w.writerow([int(now),
                    f'{user_pct:.2f}', f'{sys_pct:.2f}',
                    f'{iowait_pct:.2f}', f'{idle_pct:.2f}',
                    f'{rd_mbs:.3f}', f'{wr_mbs:.3f}'])
        fh.flush()
        prev_cpu = cur_cpu
        prev_rd, prev_wr = cur_rd, cur_wr
        prev_t   = now
PYMON
}

# ── Embedded rate-limited transform worker ────────────────────────────────────
#
# Each worker thread targets TRANSFORMS_PER_WORKER_PER_SEC.  One transform =
# software CRC32 over each of the field_count chunks of field_length bytes,
# matching one Mycelium mRoutine pass over a single record.  Workers sleep
# between transforms to respect the rate cap — they are NOT busy loops.
#
# Output CSV:  timestamp_s, total_transforms_this_second
# (per-second aggregate across all worker threads, for stddev computation)

setup_transform_script() {
    TRANSFORM_SCRIPT=$(mktemp /tmp/slack_transform_XXXXX.py)
    cat > "$TRANSFORM_SCRIPT" << 'PYTRANS'
#!/usr/bin/env python3
"""
Rate-limited transform workers simulating Mycelium mRoutine pipeline CPU cost.

Each worker runs as a separate OS process (multiprocessing.Process) so that
it occupies a dedicated CPU core and is not serialised by the CPython GIL.
Using threading.Thread instead would cap total CPU consumption at one core
regardless of worker count, making the load model incorrect.

Each transform performs field_count × (field_length // 8) Murmur3-style
integer mix rounds — pure register arithmetic, no I/O.

Usage:
  python3 <script> <n_workers> <transforms_per_worker_per_sec>
                   <field_count> <field_length> <duration_s> <out_csv>
"""
import sys, time, multiprocessing, csv

n_workers        = int(sys.argv[1])
rate_per_wkr     = int(sys.argv[2])   # transforms/sec per worker
field_count      = int(sys.argv[3])
field_length     = int(sys.argv[4])
duration_s       = int(sys.argv[5])
out_csv          = sys.argv[6]
rounds_multiplier = int(sys.argv[7]) if len(sys.argv) > 7 else 1

# Base rounds = field_count × (field_length // 8): one hash word per 8 bytes
# of field payload.  The multiplier scales this up so each transform burns
# more CPU without changing the logical transform rate.  At multiplier=1 each
# worker sleeps ~85 % of the time at 1000 Hz; at multiplier≥8 the sleep goes
# to zero and the worker saturates one full CPU core.
ROUNDS_PER_TRANSFORM = field_count * max(1, field_length // 8) * rounds_multiplier

_M1   = 0xFF51AFD7ED558CCD   # Murmur3 mix constants
_M2   = 0xC4CEB9FE1A85EC53
_MASK = 0xFFFFFFFFFFFFFFFF

def _mix(x):
    """One Murmur3-style 64-bit integer finalisation round (register-only)."""
    x = ((x ^ (x >> 33)) * _M1) & _MASK
    x = ((x ^ (x >> 33)) * _M2) & _MASK
    return x ^ (x >> 33)

MAX_SECS = duration_s + 5

def worker_main(wid, rate, dur, start_wall, per_sec_arr):
    """
    One mRoutine pipeline — runs in its own OS process, consuming a full CPU
    core.  Uses time.time() (wall clock) against the shared start_wall epoch
    for consistent timing across processes; time.perf_counter() is used only
    for intra-process rate limiting where cross-process consistency is not
    needed.
    """
    state    = wid * 0x9E3779B97F4A7C15 & _MASK
    interval = 1.0 / rate
    end_wall = start_wall + dur
    next_t   = time.perf_counter() + interval

    while time.time() < end_wall:
        # ── one transform: ROUNDS_PER_TRANSFORM mix rounds in registers ──
        # Data-dependent chain prevents the interpreter from collapsing it.
        x = state
        for _ in range(ROUNDS_PER_TRANSFORM):
            x = _mix(x)
        state = x

        # ── record into the per-second bucket (shared array, built-in lock) ──
        elapsed_s = int(time.time() - start_wall)
        if 0 <= elapsed_s < MAX_SECS:
            with per_sec_arr.get_lock():
                per_sec_arr[elapsed_s] += 1

        # ── rate limiting: sleep until next scheduled transform ──
        sleep_for = next_t - time.perf_counter()
        if sleep_for > 0:
            time.sleep(sleep_for)
        # Don't try to catch up if we fell behind — just reschedule from now.
        next_t = max(next_t + interval, time.perf_counter())

if __name__ == '__main__':
    # Shared signed-long array — one bucket per second, written by all workers.
    # multiprocessing.Array('l', ...) is backed by shared memory and provides
    # a built-in RLock via .get_lock().
    per_sec_arr = multiprocessing.Array('l', MAX_SECS)
    start_wall  = time.time()

    procs = []
    for wid in range(n_workers):
        p = multiprocessing.Process(
            target=worker_main,
            args=(wid, rate_per_wkr, duration_s, start_wall, per_sec_arr),
            daemon=True)
        procs.append(p)

    for p in procs:
        p.start()
    for p in procs:
        p.join()

    # Write per-second aggregate CSV.
    with open(out_csv, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['elapsed_s', 'transforms_this_second'])
        for s in range(duration_s):
            w.writerow([s, per_sec_arr[s]])

    total = sum(per_sec_arr[:duration_s])
    print(f"Transform workers={n_workers}, rate={rate_per_wkr}/worker/s, "
          f"rounds_per_transform={ROUNDS_PER_TRANSFORM} (×{rounds_multiplier}), "
          f"total={total}, actual_rate={total/duration_s:.1f}/s")
PYTRANS
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
    if (( n_workers > 0 )); then
        log "  Starting $n_workers in-memory transform worker(s) ..." \
            "(${n_workers} × ${TRANSFORMS_PER_WORKER} transforms/s," \
            "rounds_multiplier=${WORKER_ROUNDS_MULTIPLIER}," \
            "total=$((n_workers * TRANSFORMS_PER_WORKER)) transforms/s)"
        python3 "$TRANSFORM_SCRIPT" \
            "$n_workers" "$TRANSFORMS_PER_WORKER" \
            "$FIELD_COUNT" "$FIELD_LENGTH" \
            "$RUNTIME_SECS" "$xfm_csv" \
            "$WORKER_ROUNDS_MULTIPLIER" &
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

run_sweep() {
    sep
    log "CPU slack sweep:"
    log "  Phase 1 DB:      ${DB_PATH}"
    log "  Knee config:     ${KNEE_THREADS} RocksDB write threads"
    log "  Knee throughput: ${KNEE_XPUT} ops/s  (Phase 1 baseline — no Step 0 run)"
    log "  Record layout:   ${FIELD_COUNT} × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Rate per worker: ${TRANSFORMS_PER_WORKER} integer-mix transforms/s (no I/O)"
    log "  Run time:        ${RUNTIME_SECS}s per step (binary measures [${WARMUP_SKIP_S}, ${RUNTIME_SECS}]s)"
    log "  Stop condition:  client write throughput drops >max(${DEGRADATION_THRESHOLD}, CoV)" \
        "for ${N_CONSECUTIVE} consecutive steps"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"

    # Reuse the Phase 1 DB directly — it is already at compaction steady state.
    # No load or warm-up step needed.
    local dbpath="$DB_PATH"

    # KNEE_XPUT from Phase 1 is the degradation reference — no Step 0 needed.
    BASELINE_XPUT_OPS="$KNEE_XPUT"
    log "  Baseline (from Phase 1): ${BASELINE_XPUT_OPS} ops/s"

    # ── Sweep: add 1 worker per step, check for degradation ──────────────────
    local n_workers=0
    local consec_drops=0          # consecutive steps that exceeded the threshold
    local first_degraded_at=""    # worker count where the run started
    while true; do
        n_workers=$(( n_workers + 1 ))
        sep
        log "Step ${n_workers}: ${n_workers} transform worker(s)" \
            "(~$((n_workers * TRANSFORMS_PER_WORKER)) transforms/s total)"

        local run_dir="$sweep_dir/run_w${n_workers}"
        mkdir -p "$run_dir"

        run_one "$n_workers" "$run_dir" "$dbpath"

        # ── Degradation check ─────────────────────────────────────────────────
        # Two guards prevent spurious stops:
        #
        #  1. Stddev floor: the effective threshold is max(DEGRADATION_THRESHOLD,
        #     CoV) where CoV = stddev/mean for this step.  A compaction transient
        #     that raises the per-second variance also raises the floor, so a noisy
        #     run cannot trigger a stop that a quiet run would not.
        #
        #  2. Consecutive-step guard: the drop must exceed the effective threshold
        #     in N_CONSECUTIVE steps in a row.  A single bad step (unlucky
        #     compaction timing) resets the counter.
        if [[ -n "$LAST_XPUT_OPS" && -n "$BASELINE_XPUT_OPS" && \
              "$BASELINE_XPUT_OPS" != "0" ]]; then
            local stop drop_pct eff_thresh_pct
            read stop drop_pct eff_thresh_pct < <(python3 -c "
current  = float('${LAST_XPUT_OPS}')
baseline = float('${BASELINE_XPUT_OPS}')
thresh   = float('${DEGRADATION_THRESHOLD}')
stddev   = float('${LAST_XPUT_STDDEV}') if '${LAST_XPUT_STDDEV}' else 0.0
# CoV = stddev / mean of this run; use it as a noise floor so a high-variance
# step (active compaction) does not trigger the stop condition on its own.
cov      = stddev / current if current > 0 else 0.0
eff      = max(thresh, cov)
drop     = (baseline - current) / baseline
print('yes' if drop > eff else 'no', f'{drop*100:.1f}', f'{eff*100:.1f}')
" 2>/dev/null || echo "no 0.0 $(python3 -c "print(f'{float(\"${DEGRADATION_THRESHOLD}\")*100:.1f}')")")

            log "  Throughput: ${LAST_XPUT_OPS} ops/s" \
                " vs baseline ${BASELINE_XPUT_OPS} ops/s" \
                " (drop: ${drop_pct}%  eff.threshold: ${eff_thresh_pct}%)"

            if [[ "$stop" == "yes" ]]; then
                consec_drops=$(( consec_drops + 1 ))
                [[ -z "$first_degraded_at" ]] && first_degraded_at="$n_workers"
                log "  Above-threshold drop: ${consec_drops}/${N_CONSECUTIVE} consecutive"
                if (( consec_drops >= N_CONSECUTIVE )); then
                    log "*** ${N_CONSECUTIVE} consecutive above-threshold drops — sweep stopping ***"
                    log "*** Slack boundary: $((first_degraded_at - 1)) workers" \
                        "(= $(( (first_degraded_at - 1) * TRANSFORMS_PER_WORKER)) transforms/s)" \
                        "| degradation confirmed at ${first_degraded_at} workers ***"
                    break
                fi
            else
                if (( consec_drops > 0 )); then
                    log "  Drop cleared (was ${consec_drops} consecutive) — resetting counter"
                fi
                consec_drops=0
                first_degraded_at=""
            fi
        else
            log "  WARNING: no throughput reading for step ${n_workers} — continuing sweep"
            consec_drops=0
            first_degraded_at=""
        fi
    done

    log "Sweep complete."
}

# ── Python plotter ────────────────────────────────────────────────────────────

write_plotter() {
    PLOTTER_SCRIPT=$(mktemp /tmp/slack_plotter_XXXXX.py)
    cat > "$PLOTTER_SCRIPT" << 'PYPLOT'
#!/usr/bin/env python3
"""
cpu_slack_plotter.py

Produces: plots/cpu_slack_curve.png
          plots/cpu_slack_summary.csv

4 panels on the same X-axis (transform worker threads):
  1. Write throughput (ops/s ± σ)      — primary degradation signal
  2. Disk read + write MB/s (± σ)      — should stay flat for in-memory transforms
  3. CPU active % (± σ)                — the slack budget being consumed
  4. Actual transform throughput (± σ) — confirms workers hit their rate targets

A red dashed baseline marks the knee throughput from Phase 1.
A vertical dashed line marks the "slack boundary" — the last worker count
where throughput stays within DEGRADATION_PCT of the knee.
"""
import sys, os, re, glob
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

OUTPUT_DIR        = sys.argv[1]
WARMUP_SKIP       = int(sys.argv[2])
KNEE_XPUT         = float(sys.argv[3])          # 0 means unknown
RATE_PER_WORKER   = int(sys.argv[4])
FIELD_COUNT       = int(sys.argv[5])
FIELD_LENGTH      = int(sys.argv[6])
DEGRADATION_PCT   = float(sys.argv[7])          # e.g. 0.10

SWEEP_DIR = os.path.join(OUTPUT_DIR, "sweep")
PLOT_DIR  = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

# ── Parsers ──────────────────────────────────────────────────────────────────

def parse_ycsb_log(path):
    try:
        text = open(path).read()
        m = re.search(
            r'throughput mean:\s*([\d.eE+\-]+)\s+stddev:\s*([\d.eE+\-]+)', text)
        if m:
            return float(m.group(1)), float(m.group(2))
    except Exception as e:
        print(f"  [skip log] {path}: {e}")
    return float('nan'), float('nan')

def parse_system_csv(path, warmup_skip):
    empty = dict(cpu_compute_mean=np.nan, cpu_active_std=np.nan,
                 cpu_busy_mean=np.nan,    cpu_iowait_mean=np.nan,
                 disk_read_mean=np.nan,   disk_read_std=np.nan,
                 disk_write_mean=np.nan,  disk_write_std=np.nan)
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        steady = df[df['elapsed_s'] >= warmup_skip]
        if steady.empty:
            steady = df
        # Exclude iowait from cpu_active so it matches what `top` shows as
        # "CPU busy".  iowait = CPU idle while waiting for I/O; for RocksDB
        # with active NVMe compaction this is often 30-50 %, making
        # (100 - idle) overstate compute utilisation significantly.
        iowait     = steady['cpu_iowait_pct'] if 'cpu_iowait_pct' in steady.columns \
                     else pd.Series([0.0] * len(steady), index=steady.index)
        active_raw = 100.0 - steady['cpu_idle_pct']   # includes iowait
        active     = active_raw - iowait               # compute-only
        dr, dw = steady['disk_read_mbs'], steady['disk_write_mbs']
        return dict(
            cpu_compute_mean = active.mean(),     cpu_active_std  = active.std(ddof=1),
            cpu_busy_mean    = active_raw.mean(), cpu_iowait_mean = iowait.mean(),
            disk_read_mean   = dr.mean(),         disk_read_std   = dr.std(ddof=1),
            disk_write_mean  = dw.mean(),         disk_write_std  = dw.std(ddof=1),
        )
    except Exception as e:
        print(f"  [skip sys] {path}: {e}")
        return empty

def parse_transform_csv(path, warmup_skip, duration_s):
    """Return (mean_transforms_per_sec, std) over the steady-state window."""
    try:
        df = pd.read_csv(path)
        steady = df[df['elapsed_s'] >= warmup_skip]
        if steady.empty:
            steady = df
        counts = steady['transforms_this_second']
        return counts.mean(), counts.std(ddof=1)
    except Exception as e:
        print(f"  [skip xfm] {path}: {e}")
    return float('nan'), float('nan')

def workers_from_path(p):
    m = re.search(r'_w(\d+)', os.path.basename(p))
    return int(m.group(1)) if m else 0

# ── Collect data ─────────────────────────────────────────────────────────────

run_dirs = sorted(
    glob.glob(os.path.join(SWEEP_DIR, "run_w*")),
    key=lambda p: workers_from_path(p)
)

rows = []
for rd in run_dirs:
    k = workers_from_path(rd)
    log_path = os.path.join(rd, f"ycsb_w{k}.log")
    sys_path = os.path.join(rd, f"system_w{k}.csv")
    xfm_path = os.path.join(rd, f"transform_w{k}.csv")

    xput_mean, xput_std = parse_ycsb_log(log_path)
    sys_stats           = parse_system_csv(sys_path, WARMUP_SKIP)
    xfm_mean, xfm_std   = parse_transform_csv(xfm_path, WARMUP_SKIP, 0)

    rows.append(dict(
        workers          = k,
        total_rate_target= k * RATE_PER_WORKER,
        xput_mean        = xput_mean,
        xput_std         = xput_std,
        xfm_mean         = xfm_mean,
        xfm_std          = xfm_std,
        **sys_stats,
    ))

if not rows:
    print("No run directories found — nothing to plot.")
    import sys as _sys; _sys.exit(0)

df = pd.DataFrame(rows).sort_values('workers').reset_index(drop=True)
df.to_csv(os.path.join(PLOT_DIR, "cpu_slack_summary.csv"), index=False)
print("Summary:")
print(df[['workers','total_rate_target','xput_mean','xput_std',
          'cpu_compute_mean','cpu_busy_mean','disk_write_mean','disk_read_mean',
          'xfm_mean']].to_string(index=False))

# ── Detect slack boundary ─────────────────────────────────────────────────────
# Last worker count where throughput stays within DEGRADATION_PCT of the knee.

knee = KNEE_XPUT if KNEE_XPUT > 0 else df['xput_mean'].dropna().iloc[0]
slack_boundary = None
valid = df.dropna(subset=['xput_mean'])
for _, row in valid.iterrows():
    if (knee - row['xput_mean']) / knee <= DEGRADATION_PCT:
        slack_boundary = int(row['workers'])
    else:
        break
if slack_boundary is not None:
    print(f"Slack boundary: {slack_boundary} workers "
          f"× {RATE_PER_WORKER} transforms/worker/s"
          f"  (= {slack_boundary * RATE_PER_WORKER} transforms/s total)")

# ── Plot ──────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    'figure.dpi': 150, 'font.size': 11,
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.grid': True, 'grid.alpha': 0.35,
    'axes.labelsize': 11, 'xtick.labelsize': 10, 'ytick.labelsize': 10,
})

fig = plt.figure(figsize=(13, 15))
gs  = gridspec.GridSpec(4, 1, hspace=0.50)
ax_xput = fig.add_subplot(gs[0])
ax_disk = fig.add_subplot(gs[1], sharex=ax_xput)
ax_cpu  = fig.add_subplot(gs[2], sharex=ax_xput)
ax_xfm  = fig.add_subplot(gs[3], sharex=ax_xput)

workers = df['workers'].values

# ── dual X-axis labels: worker count (top) + total transforms/s (bottom) ────
# We use the primary axis for worker count and annotate the target rate below.

def add_slack_vline(ax):
    if slack_boundary is not None:
        ax.axvline(slack_boundary, color='purple', linestyle=':', alpha=0.7,
                   label=f'Slack boundary: {slack_boundary} workers × {RATE_PER_WORKER}/s')

# ── Panel 1: write throughput ────────────────────────────────────────────────

ax_xput.errorbar(workers, df['xput_mean'], yerr=df['xput_std'],
                 fmt='o-', color='steelblue', linewidth=2, markersize=7,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Write throughput (ops/s)')
if KNEE_XPUT > 0:
    ax_xput.axhline(KNEE_XPUT, color='crimson', linestyle='--', alpha=0.7,
                    label=f'Knee baseline ({KNEE_XPUT:.0f} ops/s)')
    ax_xput.axhline(KNEE_XPUT * (1 - DEGRADATION_PCT),
                    color='crimson', linestyle=':', alpha=0.45,
                    label=f'−{DEGRADATION_PCT*100:.0f}% threshold')
add_slack_vline(ax_xput)
ax_xput.set_ylabel('Write throughput\n(ops / sec)')
ax_xput.set_title('Write throughput vs transform workers  (± 1 σ)')
ax_xput.legend(fontsize=9, loc='lower left')

# ── Panel 2: disk read + write MB/s ─────────────────────────────────────────

ax_disk.errorbar(workers, df['disk_write_mean'], yerr=df['disk_write_std'],
                 fmt='s-', color='darkorange', linewidth=2, markersize=7,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Disk write (MB/s)')
ax_disk.errorbar(workers, df['disk_read_mean'], yerr=df['disk_read_std'],
                 fmt='D--', color='saddlebrown', linewidth=2, markersize=6,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Disk read (MB/s)')
add_slack_vline(ax_disk)
ax_disk.set_ylabel('Disk I/O\n(MB / sec)')
ax_disk.set_title('Disk read + write bandwidth vs transform workers  (± 1 σ)\n'
                  '(flat = transforms are CPU-bound in memory; rising = I/O coupling)')
ax_disk.legend(fontsize=9, loc='lower left')

# ── Panel 3: CPU utilization — two metrics ───────────────────────────────────
# cpu_busy    = 100 - idle           (includes iowait; matches iostat %util)
# cpu_compute = 100 - idle - iowait  (pure compute: user+sys+irq+softirq)
# The gap between the two lines is the iowait component — tells you how much
# of the "busy" time is the CPU stalled on I/O vs doing real arithmetic.

ax_cpu.errorbar(workers, df['cpu_busy_mean'], yerr=df['cpu_active_std'],
                fmt='s--', color='steelblue', linewidth=2, markersize=7,
                capsize=5, capthick=1.5, elinewidth=1.5,
                label='cpu_busy = 100−idle  (incl. iowait) %')
ax_cpu.errorbar(workers, df['cpu_compute_mean'], yerr=df['cpu_active_std'],
                fmt='^-', color='forestgreen', linewidth=2, markersize=7,
                capsize=5, capthick=1.5, elinewidth=1.5,
                label='cpu_compute = 100−idle−iowait  (pure compute) %')
ax_cpu.fill_between(workers, df['cpu_compute_mean'], df['cpu_busy_mean'],
                    alpha=0.15, color='steelblue', label='iowait gap')
ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.6, label='100% ceiling')
add_slack_vline(ax_cpu)
ax_cpu.set_ylabel('CPU utilization (%)')
ax_cpu.set_title('System CPU utilization vs transform workers  (± 1 σ)\n'
                 'gap = iowait (CPU stalled on I/O, not doing useful work)')
ax_cpu.legend(fontsize=9, loc='lower right')

# ── Panel 4: actual transform throughput ─────────────────────────────────────
# Confirms workers hit their rate targets; flat curves = rate limiting works.

target_rates = df['total_rate_target'].values
ax_xfm.errorbar(workers, df['xfm_mean'], yerr=df['xfm_std'],
                fmt='P-', color='mediumpurple', linewidth=2, markersize=8,
                capsize=5, capthick=1.5, elinewidth=1.5,
                label='Actual transform rate (transforms/s)')
ax_xfm.step(workers, target_rates, where='mid',
            color='mediumpurple', linestyle=':', alpha=0.55,
            label='Target rate (transforms/s)')
add_slack_vline(ax_xfm)
ax_xfm.set_ylabel('Transform rate\n(transforms / sec)')
ax_xfm.set_title('Actual vs target transform throughput  (± 1 σ)')
ax_xfm.set_xlabel('Concurrent transform worker threads')
ax_xfm.legend(fontsize=9, loc='upper left')

# ── Secondary X-axis: total transforms/s ─────────────────────────────────────
# Add tick labels showing the total transform rate below each worker-count tick.

ax_xfm.set_xticks(workers)
ax_xfm.set_xticklabels([str(w) for w in workers])

# Add a second x-axis below showing total target transforms/s.
ax2 = ax_xfm.twiny()
ax2.set_xlim(ax_xfm.get_xlim())
ax2.set_xticks(workers)
ax2.set_xticklabels([f'{int(w*RATE_PER_WORKER)}' for w in workers],
                    fontsize=8, rotation=30, ha='left')
ax2.xaxis.set_ticks_position('bottom')
ax2.xaxis.set_label_position('bottom')
ax2.spines['bottom'].set_position(('outward', 36))
ax2.set_xlabel('Total transform target (transforms / sec)', fontsize=9)

plt.setp(ax_xput.get_xticklabels(), visible=False)
plt.setp(ax_disk.get_xticklabels(), visible=False)
plt.setp(ax_cpu.get_xticklabels(), visible=False)

record_bytes = FIELD_COUNT * FIELD_LENGTH
slack_str = (f"  |  slack: ≤{slack_boundary} workers × {RATE_PER_WORKER} transforms/worker/s"
             if slack_boundary is not None else "")
fig.suptitle(
    f'RocksDB CPU slack characterization  '
    f'({FIELD_COUNT} × {FIELD_LENGTH} B = {record_bytes} B/record, '
    f'{RATE_PER_WORKER} transforms/worker/s){slack_str}',
    fontsize=12, fontweight='bold', y=0.995)

out = os.path.join(PLOT_DIR, "cpu_slack_curve.png")
plt.savefig(out, bbox_inches='tight')
plt.close()
print(f"\nPlot saved: {out}")
PYPLOT
}

run_plotter() {
    sep
    log "Generating CPU slack plots..."
    write_plotter
    # Pass WARMUP_SKIP_S so the plotter trims the system-monitor CSVs to the
    # same [WARMUP_SKIP_S, RUNTIME_SECS] steady-state window that the YCSB
    # binary uses internally.
    python3 "$PLOTTER_SCRIPT" \
        "$OUTPUT_DIR" "$WARMUP_SKIP_S" "$KNEE_XPUT" \
        "$TRANSFORMS_PER_WORKER" "$FIELD_COUNT" "$FIELD_LENGTH" \
        "$DEGRADATION_THRESHOLD"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    log "cpu_slack_sweep.sh starting"
    log "  Binary:          $BINARY"
    log "  Output dir:      $OUTPUT_DIR"
    log "  Device:          $DISK_DEVICE"
    log "  Knee threads:    $KNEE_THREADS  (RocksDB write threads per step)"
    log "  Knee xput:       ${KNEE_XPUT} ops/s  (plot annotation; 0 = use step-0 data point)"
    log "  Value layout:    ${FIELD_COUNT} fields × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Transform rate:  ${TRANSFORMS_PER_WORKER} transforms/worker/s  (in-memory CRC32)"
    log "  Worker sequence: 0 (baseline), 1, 2, 3, … (fresh YCSB + k workers each step)"
    log "  Runtime/step:    ${RUNTIME_SECS}s  (binary measures [${WARMUP_SKIP_S}, ${RUNTIME_SECS}]s window)"
    log "  Degrad. thresh:  ${DEGRADATION_THRESHOLD}  (stop when client xput drops this fraction)"
    echo ""

    check_prereqs
    setup_monitor_script
    setup_transform_script
    mkdir -p "$OUTPUT_DIR"

    run_sweep
    run_plotter

    sep
    log "Done."
    log "  Results: $OUTPUT_DIR"
    log "  Plot:    $OUTPUT_DIR/plots/cpu_slack_curve.png"
    log ""
    log "  Typical invocation for Phase 2 after reading Phase 1 results:"
    log "    KNEE_THREADS=16 KNEE_XPUT=8500 \\"
    log "    TRANSFORMS_PER_WORKER=1000 \\"
    log "    RUNTIME_SECS=120 \\"
    log "    WORKLOAD_SPEC=/path/to/workload.spec \\"
    log "    bash ../utility/cpu_slack_sweep.sh"
    log ""
    log "  Serve locally:  python3 -m http.server 8888 --directory $OUTPUT_DIR"
}

main "$@"
