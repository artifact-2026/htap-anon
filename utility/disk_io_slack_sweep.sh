#!/usr/bin/env bash
# =============================================================================
# disk_io_slack_sweep.sh — Disk I/O slack characterization (write-only)
#
# Companion to cpu_slack_sweep.sh.  At the saturation knee, the system uses
# some fraction of disk bandwidth for compaction + WAL + flushes.  This script
# quantifies the remaining disk bandwidth ("disk slack") by incrementally
# adding write-only I/O interference workers and measuring throughput
# degradation.
#
# Story: create a fresh RocksDB, preload it, then start a single YCSB
# throughput run at KNEE_THREADS.  Every XPUT_WINDOW seconds one additional
# rate-limited disk I/O writer is launched alongside the running YCSB client.
#
#   window 0 (t=  0..30):  0 IO workers — pure-YCSB baseline
#   window 1 (t= 30..60):  1 IO worker  @ IO_RATE_PER_WORKER MB/s
#   window 2 (t= 60..90):  2 IO workers
#   …
#
# IO workers:
#   - Write-only using O_DIRECT to bypass page cache and hit the actual disk
#   - Each worker writes to its own file on the SAME disk as RocksDB
#   - Rate-limited to IO_RATE_PER_WORKER MB/s per worker
#   - File size controlled by IO_DATA_SIZE_MB
#
# Run from the project BUILD directory:
#   cd /path/to/build && \
#     KNEE_THREADS=16 WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#     IO_DATA_SIZE_MB=1024 \
#     bash ../utility/disk_io_slack_sweep.sh
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   db/                        — freshly loaded RocksDB
#   sweep/
#     load.log                 — load-phase output
#     ycsb.log                 — single YCSB throughput run
#     xput_windows.csv         — per-window throughput
#     system.csv               — per-second CPU/disk samples
#     io_worker_*.dat          — interference data files (cleaned up)
#   plots/
#     disk_io_slack_curve.png  — throughput + CPU + disk over time
#     disk_io_slack_summary.csv
# =============================================================================

set -euo pipefail

# ── Optional: source Phase 1 calibration file ─────────────────────────────────
if [[ -n "${PHASE2_CONFIG:-}" ]]; then
    [[ -f "$PHASE2_CONFIG" ]] || {
        echo "ERROR: PHASE2_CONFIG not found: $PHASE2_CONFIG" >&2; exit 1; }
    # shellcheck source=/dev/null
    source "$PHASE2_CONFIG"
    echo "[$(date '+%H:%M:%S')] Sourced Phase 1 calibration: $PHASE2_CONFIG"
fi

# ── Optional: saturation summary for combined plot ────────────────────────────
SATURATION_SUMMARY="${SATURATION_SUMMARY:-}"

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./disk_io_slack_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── DB setup ──────────────────────────────────────────────────────────────────
RECORD_COUNT="${RECORD_COUNT:-5000000}"

# ── Workload ──────────────────────────────────────────────────────────────────
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"
KNEE_THREADS="${KNEE_THREADS:-16}"
KNEE_XPUT="${KNEE_XPUT:-0}"

# ── Value layout ──────────────────────────────────────────────────────────────
FIELD_LENGTH="${FIELD_LENGTH:-256}"
FIELD_COUNT="${FIELD_COUNT:-16}"

# ── IO worker knobs ───────────────────────────────────────────────────────────

# Target write bandwidth per IO worker (MB/s).
IO_RATE_PER_WORKER="${IO_RATE_PER_WORKER:-50}"

# Block size for each write() call (bytes).  Must be a multiple of 512 for
# O_DIRECT alignment.  4096 matches most filesystem block sizes.
IO_BLOCK_SIZE="${IO_BLOCK_SIZE:-4096}"

# Size of each IO worker's data file in megabytes.
# When the worker reaches the end it wraps around (sequential loop).
# Should be large enough to avoid trivial caching effects.
IO_DATA_SIZE_MB="${IO_DATA_SIZE_MB:-1024}"

# ── Experiment timing ─────────────────────────────────────────────────────────

DISK_DEVICE="${DISK_DEVICE:-}"
RUNTIME_SECS="${RUNTIME_SECS:-900}"
XPUT_WINDOW="${XPUT_WINDOW:-30}"
WARMUP_SKIP_S="${WARMUP_SKIP_S:-0}"
DEGRADATION_THRESHOLD="${DEGRADATION_THRESHOLD:-0.10}"
DROP_CACHES="${DROP_CACHES:-true}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
IO_WORKER_SCRIPT=""
PLOTTER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""
BASELINE_XPUT_OPS=""
LAST_XPUT_OPS=""
LAST_XPUT_STDDEV=""
declare -a IO_WORKER_PIDS=()
declare -a IO_WORKER_FILES=()

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    local pid
    for pid in "${IO_WORKER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    # Clean up interference data files
    local f
    for f in "${IO_WORKER_FILES[@]:-}"; do
        [[ -f "$f" ]] && rm -f "$f"
    done
    [[ -n "${MONITOR_SCRIPT:-}"    ]] && rm -f "$MONITOR_SCRIPT"
    [[ -n "${IO_WORKER_SCRIPT:-}"  ]] && rm -f "$IO_WORKER_SCRIPT"
    [[ -n "${PLOTTER_SCRIPT:-}"    ]] && rm -f "$PLOTTER_SCRIPT"
    [[ -n "${TMPSPEC:-}"           ]] && rm -f "$TMPSPEC"
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
    # Auto-detect DISK_DEVICE
    if [[ -z "$DISK_DEVICE" ]] || \
       ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        local devfile devname detected=""
        devfile=$(df "$(dirname "$OUTPUT_DIR")" 2>/dev/null | awk 'NR==2{print $1}')
        devname="${devfile##*/}"
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
            log "  DISK_DEVICE auto-detected '$detected'"
            DISK_DEVICE="$detected"
        else
            log "WARNING: DISK_DEVICE auto-detection failed — disk I/O will be zero."
        fi
    fi
    (( RUNTIME_SECS > 0 ))   || die "RUNTIME_SECS must be > 0"
    (( XPUT_WINDOW > 0 ))    || die "XPUT_WINDOW must be > 0"
    (( RUNTIME_SECS % XPUT_WINDOW == 0 )) || \
        die "RUNTIME_SECS ($RUNTIME_SECS) must be divisible by XPUT_WINDOW ($XPUT_WINDOW)"
}

# ── Embedded per-second system monitor (same as cpu_slack_sweep.sh) ───────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp /tmp/diskio_monitor_XXXXX.py)
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

def read_ctxt():
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

# ── Embedded write-only IO worker ─────────────────────────────────────────────
#
# Each worker opens a pre-allocated file with O_DIRECT and writes at a
# rate-limited bandwidth (IO_RATE_PER_WORKER MB/s).  Sequential writes wrap
# around when the file end is reached.
#
# Usage:
#   python3 <script> <data_file> <rate_mbs> <block_size> <duration_s>

setup_io_worker_script() {
    IO_WORKER_SCRIPT=$(mktemp /tmp/diskio_worker_XXXXX.py)
    cat > "$IO_WORKER_SCRIPT" << 'PYIOWORKER'
#!/usr/bin/env python3
"""
Write-only disk I/O interference worker using O_DIRECT.

Opens a pre-allocated file with O_DIRECT and performs sequential writes at
a rate-limited bandwidth.  Wraps around at EOF.  Uses posix_memalign-style
aligned buffers for O_DIRECT compatibility.

Usage:
  python3 <script> <data_file> <rate_mbs> <block_size> <duration_s>
"""
import sys, os, time, mmap, ctypes

data_file   = sys.argv[1]
rate_mbs    = float(sys.argv[2])    # target MB/s
block_size  = int(sys.argv[3])      # bytes per write
duration_s  = int(sys.argv[4])

# Validate alignment for O_DIRECT
assert block_size % 512 == 0, f"block_size must be multiple of 512, got {block_size}"

file_size = os.path.getsize(data_file)
assert file_size > 0, f"Data file is empty: {data_file}"

# Allocate an aligned write buffer (O_DIRECT requires aligned memory).
# Use ctypes to get a page-aligned buffer.
alignment = max(4096, block_size)
raw_buf = ctypes.create_string_buffer(block_size + alignment)
buf_addr = ctypes.addressof(raw_buf)
aligned_addr = (buf_addr + alignment - 1) & ~(alignment - 1)
offset_in_raw = aligned_addr - buf_addr

# Fill with a recognizable pattern (not zeros — avoids filesystem short-cuts).
pattern = bytes([0xAB] * block_size)
ctypes.memmove(aligned_addr, pattern, block_size)

# Compute inter-write sleep interval for rate limiting.
# rate_mbs MB/s → (rate_mbs * 1024 * 1024) bytes/s → interval = block_size / bytes_per_sec
bytes_per_sec = rate_mbs * 1024.0 * 1024.0
interval = block_size / bytes_per_sec if bytes_per_sec > 0 else 0.0

# Open with O_DIRECT | O_WRONLY
flags = os.O_WRONLY | os.O_DIRECT
try:
    fd = os.open(data_file, flags)
except OSError as e:
    # Fallback: O_DIRECT not supported (e.g., some filesystems)
    print(f"WARNING: O_DIRECT failed ({e}), falling back to buffered I/O", flush=True)
    fd = os.open(data_file, os.O_WRONLY)

end_time = time.time() + duration_s
pos = 0
total_bytes = 0
writes = 0

# Build a bytes object from the aligned buffer for os.write
write_buf = (ctypes.c_char * block_size).from_address(aligned_addr)
write_bytes = bytes(write_buf)

try:
    while time.time() < end_time:
        t0 = time.monotonic()

        # Wrap around at file end
        if pos + block_size > file_size:
            pos = 0
            os.lseek(fd, 0, os.SEEK_SET)

        try:
            n = os.write(fd, write_bytes)
            pos += n
            total_bytes += n
            writes += 1
        except OSError:
            # Seek to aligned position on error and retry
            pos = 0
            os.lseek(fd, 0, os.SEEK_SET)
            continue

        # Rate limiting
        elapsed = time.monotonic() - t0
        sleep_for = interval - elapsed
        if sleep_for > 0:
            time.sleep(sleep_for)

finally:
    os.close(fd)

elapsed_total = duration_s
actual_mbs = total_bytes / 1024.0 / 1024.0 / max(1, elapsed_total)
print(f"IO worker: file={data_file}, target={rate_mbs:.1f} MB/s, "
      f"actual={actual_mbs:.1f} MB/s, writes={writes}, "
      f"total={total_bytes/1024/1024:.1f} MB in {elapsed_total}s", flush=True)
PYIOWORKER
}

# ── Monitor / IO helpers ──────────────────────────────────────────────────────

start_monitor()   { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor()    {
    [[ -n "${MONITOR_PID:-}" ]] && {
        kill "$MONITOR_PID" 2>/dev/null || true; wait "$MONITOR_PID" 2>/dev/null || true; MONITOR_PID=""; }
}

# Pre-allocate an IO worker data file.
#   preallocate_io_file <path> <size_mb>
preallocate_io_file() {
    local path="$1" size_mb="$2"
    log "  Pre-allocating ${size_mb} MB IO file: $path"
    dd if=/dev/zero of="$path" bs=1M count="$size_mb" conv=fdatasync 2>/dev/null
    IO_WORKER_FILES+=("$path")
}

# ── Page-cache flush ──────────────────────────────────────────────────────────

drop_page_cache() {
    if [[ "$DROP_CACHES" != "true" ]]; then
        return
    fi
    log "  Dropping OS page cache (sync + drop_caches=3) ..."
    sudo sysctl -w vm.dirty_expire_centisecs=0 > /dev/null 2>&1 || true
    if echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
        log "  Page cache dropped."
    else
        log "  WARNING: drop_caches failed (need sudo or root)."
    fi
}

# ── Workload spec ─────────────────────────────────────────────────────────────

create_spec() {
    TMPSPEC=$(mktemp /tmp/diskio_spec_XXXXX.spec)
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────

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

# ── Main sweep ────────────────────────────────────────────────────────────────

run_sweep() {
    sep
    log "Disk I/O slack sweep (write-only, O_DIRECT):"
    log "  Knee threads:     ${KNEE_THREADS}"
    log "  IO rate/worker:   ${IO_RATE_PER_WORKER} MB/s (write-only)"
    log "  IO block size:    ${IO_BLOCK_SIZE} B"
    log "  IO data file:     ${IO_DATA_SIZE_MB} MB"
    log "  Runtime:          ${RUNTIME_SECS}s  |  window: ${XPUT_WINDOW}s" \
        "→ $((RUNTIME_SECS / XPUT_WINDOW)) windows"
    log "  Record layout:    ${FIELD_COUNT} × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  Degrad. thresh:   ${DEGRADATION_THRESHOLD} (plot annotation only)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"

    # ── Step 1: create and load fresh DB ──────────────────────────────────────
    local dbpath="$OUTPUT_DIR/db"
    log "  Creating fresh DB at $dbpath"
    load_db "$dbpath" "$sweep_dir/load.log"

    # ── Step 2: drop page cache ───────────────────────────────────────────────
    drop_page_cache

    local xput_file="$sweep_dir/xput_windows.csv"
    local sys_csv="$sweep_dir/system.csv"
    local n_windows=$(( RUNTIME_SECS / XPUT_WINDOW ))

    # ── Step 3: pre-allocate IO worker data files ─────────────────────────────
    # Each worker gets its own file to avoid contention.  Files are written to
    # the same directory as the RocksDB database (same disk).
    log "  Pre-allocating IO worker data files (up to $((n_windows - 1)) workers) ..."
    for (( w = 1; w < n_windows; w++ )); do
        local io_file="$sweep_dir/io_worker_${w}.dat"
        preallocate_io_file "$io_file" "$IO_DATA_SIZE_MB"
    done

    # Drop page cache again after pre-allocation
    drop_page_cache

    # ── Step 4: start system monitor ──────────────────────────────────────────
    start_monitor "$sys_csv"

    # ── Step 5: start YCSB in the background ──────────────────────────────────
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

    # ── Step 6: add one IO worker per window ──────────────────────────────────
    local sweep_start=$SECONDS
    IO_WORKER_PIDS=()
    sleep "$XPUT_WINDOW"   # let window 0 complete with no IO workers

    local w
    for (( w = 1; w < n_windows; w++ )); do
        if ! kill -0 "$ycsb_pid" 2>/dev/null; then
            log "  YCSB exited early — stopping at window ${w}"
            break
        fi

        local elapsed=$(( SECONDS - sweep_start ))
        local remaining=$(( RUNTIME_SECS - elapsed ))
        (( remaining <= 0 )) && break

        local io_file="$sweep_dir/io_worker_${w}.dat"
        log "  Window ${w}: launching IO writer ${w}" \
            "(~${remaining}s remaining," \
            "total ~$((w * IO_RATE_PER_WORKER)) MB/s write interference)"

        python3 "$IO_WORKER_SCRIPT" \
            "$io_file" "$IO_RATE_PER_WORKER" \
            "$IO_BLOCK_SIZE" "$remaining" &
        IO_WORKER_PIDS+=($!)

        sleep "$XPUT_WINDOW"
    done

    # ── Step 7: wait for YCSB, then clean up ──────────────────────────────────
    wait "$ycsb_pid" 2>/dev/null || true
    rm -f "$spec"; TMPSPEC=""
    stop_monitor

    local pid
    for pid in "${IO_WORKER_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    IO_WORKER_PIDS=()

    log "  → ycsb log:      $sweep_dir/ycsb.log"
    log "  → xput windows:  $xput_file"
    log "  → system csv:    $sys_csv"
    log "Sweep complete."
}

# ── Python plotter ────────────────────────────────────────────────────────────

write_plotter() {
    PLOTTER_SCRIPT=$(mktemp /tmp/diskio_plotter_XXXXX.py)
    cat > "$PLOTTER_SCRIPT" << 'PYPLOT'
#!/usr/bin/env python3
"""
disk_io_slack_plotter.py

Reads:
  sweep/xput_windows.csv   — per-window throughput
  sweep/system.csv         — per-second CPU/disk monitor output

Produces:
  plots/disk_io_slack_curve.png   — 3-panel plot
  plots/disk_io_slack_summary.csv — machine-readable per-window summary
"""
import sys, os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

OUTPUT_DIR      = sys.argv[1]
XPUT_WINDOW     = int(sys.argv[2])
KNEE_XPUT       = float(sys.argv[3])
IO_RATE_PER_WKR = float(sys.argv[4])
DEGRADATION_PCT = float(sys.argv[5])

SWEEP_DIR = os.path.join(OUTPUT_DIR, "sweep")
PLOT_DIR  = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

# ── Load xput_windows.csv ────────────────────────────────────────────────────

xput_path = os.path.join(SWEEP_DIR, "xput_windows.csv")
try:
    xw = pd.read_csv(xput_path)
    if 'window_start_sec' not in xw.columns:
        xw.columns = ['window_start_sec', 'avg_throughput', 'stddev_throughput']
except Exception as e:
    print(f"ERROR: could not read {xput_path}: {e}")
    sys.exit(1)

xw = xw.sort_values('window_start_sec').reset_index(drop=True)
xw['workers'] = xw.index.astype(int)
xw['total_io_mbs'] = xw['workers'] * IO_RATE_PER_WKR

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

    for _, wrow in xw.iterrows():
        t0 = wrow['window_start_sec']
        t1 = t0 + XPUT_WINDOW
        win = sys_df[(sys_df['elapsed_s'] >= t0) & (sys_df['elapsed_s'] < t1)]
        if win.empty:
            sys_rows.append(dict(cpu_compute_mean=np.nan, cpu_busy_mean=np.nan,
                                 cpu_iowait_mean=np.nan,
                                 cpu_compute_std=np.nan,  cpu_busy_std=np.nan,
                                 disk_read_mean=np.nan,   disk_read_std=np.nan,
                                 disk_write_mean=np.nan,  disk_write_std=np.nan))
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
            ))
except Exception as e:
    print(f"  [warn] Could not parse {sys_path}: {e}")
    sys_rows = [dict(cpu_compute_mean=np.nan, cpu_busy_mean=np.nan,
                     cpu_iowait_mean=np.nan,
                     disk_read_mean=np.nan, disk_write_mean=np.nan)] * len(xw)

sys_agg = pd.DataFrame(sys_rows)
df = pd.concat([xw.reset_index(drop=True), sys_agg.reset_index(drop=True)], axis=1)

df.to_csv(os.path.join(PLOT_DIR, "disk_io_slack_summary.csv"), index=False)
print("Per-window summary:")
print(df[['window_start_sec','workers','total_io_mbs',
          'avg_throughput','stddev_throughput',
          'cpu_compute_mean','cpu_busy_mean',
          'disk_write_mean','disk_read_mean']].to_string(index=False))

# ── Detect slack boundary ────────────────────────────────────────────────────

baseline_xput = KNEE_XPUT if KNEE_XPUT > 0 else df['avg_throughput'].dropna().iloc[0]
slack_boundary_w = None
valid = df.dropna(subset=['avg_throughput'])
for _, row in valid.iterrows():
    if baseline_xput > 0 and (baseline_xput - row['avg_throughput']) / baseline_xput <= DEGRADATION_PCT:
        slack_boundary_w = int(row['workers'])
    else:
        if slack_boundary_w is not None:
            break

if slack_boundary_w is not None:
    print(f"Disk IO slack boundary: {slack_boundary_w} workers "
          f"× {IO_RATE_PER_WKR:.0f} MB/s/worker"
          f"  (= {slack_boundary_w * IO_RATE_PER_WKR:.0f} MB/s total)")

# ── Plot ──────────────────────────────────────────────────────────────────────

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
            lbl = f'IO slack boundary: {slack_boundary_w} writers' if label else None
            ax.axvline(vx, color='purple', linestyle=':', alpha=0.75, label=lbl)

# ── Panel 1: write throughput ────────────────────────────────────────────────

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
ax_xput.set_title('Write throughput vs concurrent disk IO writers  (± 1σ)')
ax_xput.legend(fontsize=9, loc='lower left')

# ── Panel 2: CPU utilization ─────────────────────────────────────────────────

ax_cpu.errorbar(t, df['cpu_busy_mean'], yerr=df.get('cpu_busy_std', pd.Series(0.0)),
                fmt='s--', color='steelblue', linewidth=2, markersize=6,
                capsize=4, capthick=1.5, elinewidth=1.5,
                label='cpu_busy = 100−idle  (incl. iowait) %')
ax_cpu.errorbar(t, df['cpu_compute_mean'], yerr=df.get('cpu_compute_std', pd.Series(0.0)),
                fmt='^-', color='forestgreen', linewidth=2, markersize=6,
                capsize=4, capthick=1.5, elinewidth=1.5,
                label='cpu_compute = 100−idle−iowait %')
ax_cpu.fill_between(t, df['cpu_compute_mean'], df['cpu_busy_mean'],
                    alpha=0.13, color='steelblue', label='iowait gap')
ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.55, label='100% ceiling')
add_slack_vline(ax_cpu, label=False)
ax_cpu.set_ylabel('CPU utilization (%)')
ax_cpu.set_title('System CPU utilization  (± 1σ)  —  gap = iowait')
ax_cpu.legend(fontsize=9, loc='lower right')

# ── Panel 3: disk bandwidth ──────────────────────────────────────────────────

ax_disk.errorbar(t, df['disk_write_mean'], yerr=df.get('disk_write_std', pd.Series(0.0)),
                 fmt='s-', color='darkorange', linewidth=2, markersize=6,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Disk write (MB/s)  ± 1σ')
ax_disk.errorbar(t, df['disk_read_mean'], yerr=df.get('disk_read_std', pd.Series(0.0)),
                 fmt='D--', color='saddlebrown', linewidth=2, markersize=5,
                 capsize=4, capthick=1.5, elinewidth=1.5,
                 label='Disk read (MB/s)  ± 1σ')
add_slack_vline(ax_disk, label=False)
ax_disk.set_ylabel('Disk I/O\n(MB / sec)')
ax_disk.set_title('Disk bandwidth  (± 1σ)')
ax_disk.legend(fontsize=9, loc='upper left')

# ── X-axis ───────────────────────────────────────────────────────────────────

tick_labels = [f'w={int(w)}\n({int(w)*IO_RATE_PER_WKR:.0f} MB/s)' for w in wkrs]
ax_disk.set_xticks(t)
ax_disk.set_xticklabels(tick_labels, rotation=45, ha='right', fontsize=7)
ax_disk.set_xlabel('Concurrent IO writers  (w=0 = baseline)', fontsize=10)

plt.setp(ax_xput.get_xticklabels(), visible=False)
plt.setp(ax_cpu.get_xticklabels(),  visible=False)

slack_str = (f"  |  IO slack ≤ {slack_boundary_w} writers × {IO_RATE_PER_WKR:.0f} MB/s"
             if slack_boundary_w is not None else "")
fig.suptitle(
    f'RocksDB disk I/O slack — incremental writer sweep  '
    f'(write-only, O_DIRECT, {IO_RATE_PER_WKR:.0f} MB/s/writer){slack_str}',
    fontsize=12, fontweight='bold', y=0.998)

out = os.path.join(PLOT_DIR, "disk_io_slack_curve.png")
plt.savefig(out, bbox_inches='tight')
plt.close()
print(f"\nPlot saved: {out}")
PYPLOT
}

run_plotter() {
    sep
    log "Generating disk I/O slack plots..."
    write_plotter
    python3 "$PLOTTER_SCRIPT" \
        "$OUTPUT_DIR" "$XPUT_WINDOW" "$KNEE_XPUT" \
        "$IO_RATE_PER_WORKER" "$DEGRADATION_THRESHOLD"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    log "disk_io_slack_sweep.sh starting  (write-only O_DIRECT mode)"
    log "  Binary:          $BINARY"
    log "  Output dir:      $OUTPUT_DIR"
    log "  Device:          $DISK_DEVICE"
    log "  Knee threads:    $KNEE_THREADS  (RocksDB write threads)"
    log "  Knee xput:       ${KNEE_XPUT} ops/s"
    log "  DB path:         $OUTPUT_DIR/db  (created fresh)"
    log "  Record count:    ${RECORD_COUNT}"
    log "  Value layout:    ${FIELD_COUNT} fields × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record"
    log "  IO rate/worker:  ${IO_RATE_PER_WORKER} MB/s  (write-only, O_DIRECT)"
    log "  IO block size:   ${IO_BLOCK_SIZE} B"
    log "  IO data file:    ${IO_DATA_SIZE_MB} MB"
    log "  Runtime:         ${RUNTIME_SECS}s  ÷  window ${XPUT_WINDOW}s  = $((RUNTIME_SECS / XPUT_WINDOW)) windows"
    log "  Worker schedule: +1 IO writer per window (window 0 = baseline)"
    log "  Degrad. thresh:  ${DEGRADATION_THRESHOLD}  (plot annotation only)"
    log "  Sat. summary:    ${SATURATION_SUMMARY:-<not set>}"
    echo ""

    check_prereqs
    setup_monitor_script
    setup_io_worker_script
    mkdir -p "$OUTPUT_DIR"

    run_sweep
    run_plotter

    sep
    log "Done."
    log "  Results:        $OUTPUT_DIR/sweep/"
    log "  Plot:           $OUTPUT_DIR/plots/disk_io_slack_curve.png"
    log "  Summary CSV:    $OUTPUT_DIR/plots/disk_io_slack_summary.csv"
    log "  Per-window CSV: $OUTPUT_DIR/sweep/xput_windows.csv"
    log ""
    log "  Minimal invocation:"
    log "    KNEE_THREADS=16 \\"
    log "    WORKLOAD_SPEC=/path/to/workload.spec \\"
    log "    IO_DATA_SIZE_MB=1024 \\"
    log "    bash ../utility/disk_io_slack_sweep.sh"
}

main "$@"
