#!/usr/bin/env bash
# =============================================================================
# cpu_slack_sweep.sh — Phase 2: CPU slack characterization
#
# Story: at the knee operating point found by saturation_sweep.sh, run YCSB
# writers at the knee thread count while concurrently increasing the load from
# rate-limited transform worker threads.  Each worker simulates one Mycelium
# mRoutine pipeline: it processes one record-worth of data (field_count ×
# field_length bytes) per transform using software CRC32, sleeping between
# transforms to hit a fixed rate target.  The sweep is over the number of
# concurrent worker threads.
#
# Because transforms in production fire once per compacted record, the per-
# worker rate is set to TRANSFORMS_PER_WORKER_PER_SEC (default 1000/s).
# Total transform load at step k = k × TRANSFORMS_PER_WORKER_PER_SEC.
# The claim produced: "baseline absorbs up to K transform workers
# (= K × R transforms/s) before write throughput degrades."
#
# Run from the project BUILD directory:
#   cd /path/to/build && bash ../utility/cpu_slack_sweep.sh
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
DB_BASE_PATH="${DB_BASE_PATH:-/holly/slack_exp_db}"
OUTPUT_DIR="${OUTPUT_DIR:-./slack_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── Phase 1 results — set these ───────────────────────────────────────────────

# ── Workload ──────────────────────────────────────────────────────────────────
# WORKLOAD_SPEC: path to a YCSB .spec file (required — no default).
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"

# Number of YCSB writer threads at the knee (from saturation_sweep.sh output).
KNEE_THREADS="${KNEE_THREADS:-16}"

# Knee throughput in ops/sec (used only for the baseline annotation in the plot).
KNEE_XPUT="${KNEE_XPUT:-0}"

# ── Value layout — must match saturation_sweep.sh ─────────────────────────────

FIELD_LENGTH="${FIELD_LENGTH:-256}"
FIELD_COUNT="${FIELD_COUNT:-16}"

# ── Transform worker knobs ────────────────────────────────────────────────────

# Seconds to measure baseline disk write rate after RocksDB warmup.
# A wider window smooths compaction bursts; 60 s is usually sufficient.
BASELINE_MEASURE_S="${BASELINE_MEASURE_S:-60}"

# Target transform rate per worker thread (transforms / second).
# Each transform processes field_count * field_length bytes via software CRC32.
# At 16 × 256 B = 4096 B/transform and 1000 transforms/s per worker,
# one worker reads ~4 MB/s of in-memory data — realistic for a single
# mRoutine pipeline.  Set relative to your measured compaction output rate.
TRANSFORMS_PER_WORKER="${TRANSFORMS_PER_WORKER:-1000}"

# ── Experiment timing ─────────────────────────────────────────────────────────

RUNTIME_SECS="${RUNTIME_SECS:-300}"
RECORD_COUNT="${RECORD_COUNT:-5000000}"
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Seconds to skip at the start when computing CPU/disk stats (matches YCSB's
# internal 60 s warmup skip in runXput).
WARMUP_SKIP_S="${WARMUP_SKIP_S:-60}"

# Throughput degradation threshold: stop the sweep (with a warning) if YCSB
# throughput drops more than this fraction below the knee.
DEGRADATION_THRESHOLD="${DEGRADATION_THRESHOLD:-0.10}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
TRANSFORM_SCRIPT=""
PLOTTER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""
TRANSFORM_PID=""
ROCKSDB_BG_PID=""
ROCKSDB_BG_LOG=""
BASELINE_DISK_WR_MBS=""
LAST_DISK_WR_MBS=""

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}"      ]] && kill "$MONITOR_PID"      2>/dev/null || true
    [[ -n "${TRANSFORM_PID:-}"    ]] && kill "$TRANSFORM_PID"    2>/dev/null || true
    [[ -n "${ROCKSDB_BG_PID:-}"   ]] && kill "$ROCKSDB_BG_PID"   2>/dev/null || true
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
Rate-limited transform workers simulating Mycelium mRoutine pipelines.

Usage:
  python3 <script> <n_workers> <transforms_per_worker_per_sec>
                   <field_count> <field_length> <duration_s> <out_csv>
"""
import sys, time, threading, binascii, csv

n_workers    = int(sys.argv[1])
rate_per_wkr = int(sys.argv[2])   # transforms/sec per worker
field_count  = int(sys.argv[3])
field_length = int(sys.argv[4])
duration_s   = int(sys.argv[5])
out_csv      = sys.argv[6]

record_size = field_count * field_length

# Per-second counter shared across threads (one bucket per second).
# Pre-allocate for the full run duration + a small buffer.
MAX_SECS = duration_s + 5
per_sec_counts = [0] * MAX_SECS
count_lock = threading.Lock()

start_time = time.perf_counter()

def worker(wid, rate, dur):
    """Single mRoutine pipeline: CRC32 over each field chunk, rate-limited."""
    # Give each worker a private buffer — avoids false sharing.
    buf = bytearray(record_size)
    # Fill with pseudo-random bytes so CRC32 does real work.
    for i in range(record_size):
        buf[i] = (wid * 7 + i * 13) & 0xFF

    interval = 1.0 / rate   # target seconds between transforms
    end      = start_time + dur
    next_t   = time.perf_counter() + interval

    while True:
        now = time.perf_counter()
        if now >= end:
            break

        # ── one transform: CRC32 over each field chunk ──
        offset = 0
        for _ in range(field_count):
            binascii.crc32(buf, offset)   # hardware-accelerated if available
            offset += field_length

        # ── record into the per-second bucket ──
        elapsed_s = int(time.perf_counter() - start_time)
        if 0 <= elapsed_s < MAX_SECS:
            with count_lock:
                per_sec_counts[elapsed_s] += 1

        # ── rate limiting: sleep until next scheduled transform ──
        sleep_for = next_t - time.perf_counter()
        if sleep_for > 0:
            time.sleep(sleep_for)
        # If we fell behind, don't try to catch up — just reschedule from now.
        next_t = max(next_t + interval, time.perf_counter())

threads = []
for wid in range(n_workers):
    t = threading.Thread(target=worker,
                         args=(wid, rate_per_wkr, duration_s),
                         daemon=True)
    threads.append(t)

for t in threads:
    t.start()
for t in threads:
    t.join()

# Write per-second aggregate CSV.
with open(out_csv, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['elapsed_s', 'transforms_this_second'])
    for s, cnt in enumerate(per_sec_counts[:duration_s]):
        w.writerow([s, cnt])

total = sum(per_sec_counts[:duration_s])
print(f"Transform workers={n_workers}, rate={rate_per_wkr}/worker/s, "
      f"total={total}, actual_rate={total/duration_s:.1f}/s")
PYTRANS
}

# ── Monitor / transform helpers ───────────────────────────────────────────────

start_monitor()   { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor()    {
    [[ -n "${MONITOR_PID:-}" ]] && {
        kill "$MONITOR_PID" 2>/dev/null || true; wait "$MONITOR_PID" 2>/dev/null || true; MONITOR_PID=""; }
}

start_transforms() {
    local n_workers="$1" out_csv="$2"
    if (( n_workers == 0 )); then
        TRANSFORM_PID=""
        return
    fi
    python3 "$TRANSFORM_SCRIPT" \
        "$n_workers" "$TRANSFORMS_PER_WORKER" \
        "$FIELD_COUNT" "$FIELD_LENGTH" \
        "$RUNTIME_SECS" "$out_csv" &
    TRANSFORM_PID=$!
}

stop_transforms() {
    if [[ -n "${TRANSFORM_PID:-}" ]]; then
        # Workers are daemon threads; the process exits naturally at duration_s.
        wait "$TRANSFORM_PID" 2>/dev/null || true
        TRANSFORM_PID=""
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

load_db() {
    local dbpath="$1"
    local rc; rc=$(grep -E '^recordcount\s*=' "$WORKLOAD_SPEC" | tail -1 | cut -d= -f2 | tr -d ' ')
    
    # ── Step 1: load phase ────────────────────────────────────────────────────
    log "Loading ${rc:-unknown} records into $dbpath ..."
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap true -threads 8 \
        -load true -run false -throughput false \
        -runtime 0 \
        -levels 7 -table baseline \
        2>&1 | tee "$OUTPUT_DIR/load2.log"
    log "Load complete."

    # ── Step 2: start indefinite background throughput run ────────────────────
    # runtime=999999 (≈11.5 days) makes it effectively indefinite for the sweep.
    # The process is killed by cleanup() on EXIT.
    ROCKSDB_BG_LOG="$OUTPUT_DIR/rocksdb_bg_throughput.log"
    log "Starting background RocksDB throughput: $KNEE_THREADS threads (indefinite) ..."
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$bg_spec" \
        -bootstrap false -threads "$KNEE_THREADS" \
        -load false -run false -throughput true \
        -runtime 999999 \
        -levels 7 -table baseline \
        > "$ROCKSDB_BG_LOG" 2>&1 &
    ROCKSDB_BG_PID=$!
    rm -f "$spec"; TMPSPEC=""
    log "  Background RocksDB PID: $ROCKSDB_BG_PID  (log: $ROCKSDB_BG_LOG)"
}

# ── Single transform-worker-count run ─────────────────────────────────────────

run_one() {
    local n_workers="$1" run_dir="$2"
    # NOTE: RocksDB runs in the background (started by load_db).
    # This function runs only in-memory operations: system monitor + CRC32
    # transform workers.  No YCSB binary is launched here.

    local sys_csv="$run_dir/system_w${n_workers}.csv"
    local xfm_csv="$run_dir/transform_w${n_workers}.csv"
    local log_file="$run_dir/ycsb_w${n_workers}.log"

    touch "$xfm_csv"
    : > "$log_file"   # stub; throughput line written below for plotter compat

    log "  Starting system monitor ..."
    start_monitor "$sys_csv"

    log "  Starting $n_workers transform worker(s) at ${TRANSFORMS_PER_WORKER} transforms/s each ..."
    start_transforms "$n_workers" "$xfm_csv"

    stop_transforms   # blocks until RUNTIME_SECS have elapsed

    stop_monitor

    # ── Compute steady-state disk write MB/s from this step's system CSV ──────
    # The background RocksDB is the only process writing to disk, so this
    # directly reflects its write throughput while transform workers were active.
    LAST_DISK_WR_MBS=$(python3 - <<PYEOF
import csv, sys
try:
    rows = []
    with open('$sys_csv') as f:
        for row in csv.DictReader(f):
            rows.append(float(row.get('disk_write_mbs', 0)))
    steady = rows[$WARMUP_SKIP_S:] if len(rows) > $WARMUP_SKIP_S else rows
    print(f'{sum(steady)/len(steady):.3f}' if steady else '0')
except Exception:
    print('0')
PYEOF
)

    # Write a throughput stub so the plotter can parse this run directory.
    # Convert disk MB/s → approximate ops/s (amplification assumed constant;
    # only the relative drop matters for the sweep stop condition).
    local bytes_per_rec=$(( FIELD_COUNT * FIELD_LENGTH ))
    local approx_ops
    approx_ops=$(python3 -c "
mbs = float('${LAST_DISK_WR_MBS}')
ops = mbs * 1024 * 1024 / max(${bytes_per_rec}, 1)
print(f'{ops:.2f}')
" 2>/dev/null || echo "0")
    printf 'throughput mean: %s  stddev: 0.0\n' "$approx_ops" >> "$log_file"

    log "  → system csv:    $sys_csv"
    log "  → transform csv: $xfm_csv"
    log "  → disk write:    ${LAST_DISK_WR_MBS} MB/s  (~${approx_ops} ops/s proxy)"
}

# ── Main sweep ────────────────────────────────────────────────────────────────

run_sweep() {
    sep
    log "CPU slack sweep:"
    log "  Knee config:     ${KNEE_THREADS} RocksDB write threads (background, indefinite)"
    log "  Record layout:   ${FIELD_COUNT} × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Rate per worker: ${TRANSFORMS_PER_WORKER} in-memory transforms/s (CRC32)"
    log "  Runtime/step:    ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    log "  Stop condition:  disk write throughput drops >${DEGRADATION_THRESHOLD} ($(python3 -c \
        "print(f'{${DEGRADATION_THRESHOLD}*100:.0f}%')" 2>/dev/null || echo "${DEGRADATION_THRESHOLD}") below baseline"
    log "  Worker sequence: 1, 2, 3, … (auto-increment until degradation detected)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"
    local dbpath="$sweep_dir/rocksdb"

    # load_db: (1) loads records, (2) starts indefinite background throughput,
    #           (3) measures BASELINE_DISK_WR_MBS after warmup.
    load_db "$dbpath"

    local n_workers=0
    local max_transform_workers=32
    while n_workders < $max_transform_workders; do
        n_workers=$(( n_workers + 1 ))
        sep
        log "Step ${n_workers}: ${n_workers} transform worker(s)  (total rate: $((n_workers * TRANSFORMS_PER_WORKER)) transforms/s)"

        local run_dir="$sweep_dir/run_w${n_workers}"
        mkdir -p "$run_dir"

        # run_one: in-memory CRC32 load + system monitor; sets LAST_DISK_WR_MBS.
        run_one "$n_workers" "$run_dir"

        # ── Degradation check ────────────────────────────────────────────────
        # Compare disk write MB/s (proxy for RocksDB write throughput) to
        # baseline measured in load_db.  Since transform workers are purely
        # in-memory (no disk I/O), any drop in disk write rate is caused solely
        # by CPU contention from the transform workers stealing cores.
        if [[ -n "$LAST_DISK_WR_MBS" && -n "$BASELINE_DISK_WR_MBS" && \
              "$BASELINE_DISK_WR_MBS" != "0" ]]; then
            local stop
            stop=$(python3 -c "
current  = float('${LAST_DISK_WR_MBS}')
baseline = float('${BASELINE_DISK_WR_MBS}')
thresh   = float('${DEGRADATION_THRESHOLD}')
drop     = (baseline - current) / baseline
print('yes' if drop > thresh else 'no')
" 2>/dev/null || echo "no")

            if [[ "$stop" == "yes" ]]; then
                log "*** Write throughput dropped >${DEGRADATION_THRESHOLD}:" \
                    "${LAST_DISK_WR_MBS} MB/s vs baseline ${BASELINE_DISK_WR_MBS} MB/s ***"
                log "*** Slack boundary: $((n_workers - 1)) workers" \
                    "(degraded at ${n_workers} workers =" \
                    "$((n_workers * TRANSFORMS_PER_WORKER)) transforms/s) ***"
                break
            fi
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
    empty = dict(cpu_active_mean=np.nan, cpu_active_std=np.nan,
                 disk_read_mean=np.nan,  disk_read_std=np.nan,
                 disk_write_mean=np.nan, disk_write_std=np.nan)
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        steady = df[df['elapsed_s'] >= warmup_skip]
        if steady.empty:
            steady = df
        active = 100.0 - steady['cpu_idle_pct']
        dr, dw = steady['disk_read_mbs'], steady['disk_write_mbs']
        return dict(
            cpu_active_mean  = active.mean(), cpu_active_std  = active.std(ddof=1),
            disk_read_mean   = dr.mean(),     disk_read_std   = dr.std(ddof=1),
            disk_write_mean  = dw.mean(),     disk_write_std  = dw.std(ddof=1),
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
          'cpu_active_mean','disk_write_mean','disk_read_mean',
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

# ── Panel 3: CPU active % ────────────────────────────────────────────────────

ax_cpu.errorbar(workers, df['cpu_active_mean'], yerr=df['cpu_active_std'],
                fmt='^-', color='forestgreen', linewidth=2, markersize=7,
                capsize=5, capthick=1.5, elinewidth=1.5,
                label='CPU active (100 − idle) %')
ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.6, label='100% ceiling')
add_slack_vline(ax_cpu)
ax_cpu.set_ylabel('CPU utilization\n(100 − idle, %)')
ax_cpu.set_title('System CPU utilization vs transform workers  (± 1 σ)')
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
    log "  Knee threads:    $KNEE_THREADS  (RocksDB throughput threads, run indefinitely)"
    log "  Knee xput:       ${KNEE_XPUT} ops/s  (plot annotation; 0 = use first data point)"
    log "  Value layout:    ${FIELD_COUNT} fields × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Transform rate:  ${TRANSFORMS_PER_WORKER} transforms/worker/s  (in-memory CRC32)"
    log "  Worker sequence: 1, 2, 3, … (auto-increment; no pre-defined list)"
    log "  Runtime/step:    ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    log "  Baseline meas.:  ${BASELINE_MEASURE_S}s  (disk write rate after warmup)"
    log "  Degrad. thresh:  ${DEGRADATION_THRESHOLD}  (stop when disk write drops this fraction)"
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
    log "    BASELINE_MEASURE_S=60 \\"
    log "    bash ../utility/cpu_slack_sweep.sh"
    log ""
    log "  Serve locally:  python3 -m http.server 8888 --directory $OUTPUT_DIR"
}

main "$@"
