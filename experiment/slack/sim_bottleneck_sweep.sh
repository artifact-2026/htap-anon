#!/usr/bin/env bash
# =============================================================================
# sim_bottleneck_sweep.sh — Simulate a bottlenecked RocksDB+YCSB system using
#                           iBench_cpu and iBench_io, then sweep in-memory
#                           split transform workers to measure achievable
#                           transformation throughput.
#
# Motivation:
#   Running a full RocksDB+YCSB experiment to measure transformation slack is
#   expensive.  This script approximates the bottlenecked system by:
#     - iBench_cpu: KNEE_CPU_CORES busy-loop threads → simulates YCSB CPU load
#     - iBench_io:  KNEE_IO_WORKERS read+write units → simulates RocksDB I/O
#
#   On top of this background load, split_bench workers are swept from 0 to
#   MAX_SPLIT_WORKERS, each performing in-memory CSV splits (2 KiB → 2×1 KiB,
#   16 fields split into 8+8).  No disk I/O is performed by the split workers.
#
# What the sweep answers:
#   "Under a system already loaded to the knee, how many in-memory split
#    transforms/second can we additionally sustain, and at what worker count
#    does throughput plateau (CPU slack exhausted)?"
#
# Expected output shape:
#   Workers 0-K:  splits/s scales linearly  (slack is available)
#   Workers K+1+: splits/s plateaus          (remaining CPU slack exhausted)
#   → K workers × splits/s/worker = maximum sustainable transform rate
#
# Caveat:
#   iBench_cpu is pure register work; real YCSB threads sleep on I/O and
#   receive CFS sleeper-fairness boosts when they wake.  The background load
#   here is therefore a conservative approximation — it is slightly harsher
#   than real YCSB, so the measured slack may be a small underestimate.
#
# Usage:
#   cd /path/to/build
#   KNEE_CPU_CORES=12 KNEE_IO_MB_S=350 IO_DIR=/mnt/rocksdb-disk \
#     bash ../experiment/slack/sim_bottleneck_sweep.sh
#
# Required:
#   KNEE_CPU_CORES   CPU cores consumed by YCSB at the knee.
#                    Read from saturation_sweep.sh summary.csv knee row:
#                      100 - cpu_idle_pct  → cores ≈ (busy_pct/100) × nproc
#
#   KNEE_IO_MB_S     Total disk bandwidth at the knee in MB/s (read + write).
#                    Read directly from the knee row of summary.csv:
#                      disk_read_mb/s + disk_write_mb/s
#                    The script runs a short iBench_io calibration (15s) to
#                    measure single-worker bandwidth on this device, then
#                    computes KNEE_IO_WORKERS = ceil(KNEE_IO_MB_S / single_mbs).
#
#                    Alternative: set KNEE_IO_WORKERS directly if you already
#                    know the right value from a prior calibration run.
#
# Optional:
#   KNEE_IO_WORKERS  Override: set number of iBench_io workers directly,
#                    skipping the calibration step.
#   IO_DIR           Directory for iBench_io temp files; MUST be on the same
#                    physical disk as RocksDB (default: /tmp)
#   IO_FILE_MB       iBench_io file size per worker in MiB (default: 512).
#   MAX_SPLIT_WORKERS  maximum split workers to sweep (default: 16)
#   XPUT_WINDOW      seconds per measurement window (default: 10)
#   OUTPUT_DIR       results directory (default: ./sim_bottleneck_<timestamp>)
#
# Output files:
#   <OUTPUT_DIR>/results.csv    — per-window: workers, splits_total, splits/s
#   <OUTPUT_DIR>/system.csv     — per-second CPU% and disk MB/s
#   <OUTPUT_DIR>/plot.png       — splits/s vs worker count curve (if matplotlib)
# =============================================================================

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────

KNEE_CPU_CORES="${KNEE_CPU_CORES:-4}"

# KNEE_IO_MB_S: total disk bandwidth (read + write) consumed by RocksDB at the
# knee, in MB/s.  Read this from the knee row of saturation_sweep.sh's
# summary.csv:  disk_read_mb/s + disk_write_mb/s.
#
# When set, the script runs a short iBench_io calibration to measure how much
# bandwidth one worker generates on this device, then computes:
#   KNEE_IO_WORKERS = ceil(KNEE_IO_MB_S / single_worker_mb_s)
#
# When left empty (default), set KNEE_IO_WORKERS directly instead.
KNEE_IO_MB_S="${KNEE_IO_MB_S:-}"

# KNEE_IO_WORKERS: fallback when KNEE_IO_MB_S is not set.  If you know the
# right value from a prior calibration run, set it here directly.
KNEE_IO_WORKERS="${KNEE_IO_WORKERS:-}"

IO_DIR="${IO_DIR:-/tmp}"
IO_FILE_MB="${IO_FILE_MB:-512}"

MAX_SPLIT_WORKERS="${MAX_SPLIT_WORKERS:-16}"
XPUT_WINDOW="${XPUT_WINDOW:-10}"

OUTPUT_DIR="${OUTPUT_DIR:-./sim_bottleneck_$(date +%Y%m%d_%H%M%S)}"

# Source locations (relative to this script's directory)
SLACK_DIR="$(cd "$(dirname "$0")" && pwd)"
IBENCH_CPU_SRC="${IBENCH_CPU_SRC:-$SLACK_DIR/iBench_cpu.cc}"
IBENCH_IO_SRC="${IBENCH_IO_SRC:-$SLACK_DIR/iBench_io.cc}"
SPLIT_BENCH_SRC="${SPLIT_BENCH_SRC:-$SLACK_DIR/split_bench.cc}"

# Total background duration: settle(3s) + all windows + small buffer
TOTAL_SECS=$(( 3 + (MAX_SPLIT_WORKERS + 1) * XPUT_WINDOW + 5 ))

# =============================================================================

IBENCH_CPU_BIN=""
IBENCH_IO_BIN=""
SPLIT_BENCH_BIN=""
IBENCH_CPU_PID=""
IBENCH_IO_PID=""
MONITOR_PID=""
MONITOR_SCRIPT=""

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
sep()  { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${IBENCH_CPU_PID:-}" ]] && kill "$IBENCH_CPU_PID" 2>/dev/null || true
    [[ -n "${IBENCH_IO_PID:-}"  ]] && kill "$IBENCH_IO_PID"  2>/dev/null || true
    [[ -n "${MONITOR_PID:-}"    ]] && kill "$MONITOR_PID"    2>/dev/null || true
    [[ -n "${IBENCH_CPU_BIN:-}" ]] && rm -f "$IBENCH_CPU_BIN"
    [[ -n "${IBENCH_IO_BIN:-}"  ]] && rm -f "$IBENCH_IO_BIN"
    [[ -n "${SPLIT_BENCH_BIN:-}"]] && rm -f "$SPLIT_BENCH_BIN"
    [[ -n "${MONITOR_SCRIPT:-}" ]] && rm -f "$MONITOR_SCRIPT"
}
trap cleanup EXIT

# ── Compile ───────────────────────────────────────────────────────────────────

# ── IO worker calibration ─────────────────────────────────────────────────────
#
# Runs iBench_io with 1 worker for CALIB_SECS seconds, parses its reported
# total MB/s, and computes how many workers are needed to reproduce the knee
# I/O bandwidth (KNEE_IO_MB_S).
#
# Sets KNEE_IO_WORKERS as a side-effect.

calibrate_io_workers() {
    local calib_secs=15
    local calib_file_mb=512   # smaller file for quick calibration

    log "Calibrating iBench_io: running 1 worker × ${calib_secs}s on ${IO_DIR} ..."
    log "  (measuring single-worker bandwidth to derive KNEE_IO_WORKERS)"

    local calib_out
    calib_out=$(mktemp /tmp/sim_calib_XXXXX.txt)

    # Run 1 worker, capture stdout (iBench_io prints summary on exit)
    "$IBENCH_IO_BIN" "$calib_secs" 1 2097152 "$IO_DIR" "$calib_file_mb" \
        > "$calib_out" 2>&1 || true

    # Parse "total: X.X MB/s" from iBench_io summary output
    local single_mbs
    single_mbs=$(grep -oP 'total:\s+\K[0-9.]+' "$calib_out" | head -1 || echo "")
    cat "$calib_out" >> "$OUTPUT_DIR/ibench_io_calib.log" 2>/dev/null || true
    rm -f "$calib_out"

    if [[ -z "$single_mbs" || "$single_mbs" == "0" ]]; then
        log "WARNING: iBench_io calibration produced no readable output."
        log "         Falling back to KNEE_IO_WORKERS=1."
        KNEE_IO_WORKERS=1
        return
    fi

    log "  Single iBench_io worker achieved: ${single_mbs} MB/s (read+write total)"
    log "  Target knee I/O bandwidth:        ${KNEE_IO_MB_S} MB/s"

    # KNEE_IO_WORKERS = ceil(KNEE_IO_MB_S / single_mbs), minimum 1
    KNEE_IO_WORKERS=$(python3 -c "
import math
target = float('${KNEE_IO_MB_S}')
single = float('${single_mbs}')
if single <= 0:
    print(1)
else:
    print(max(1, math.ceil(target / single)))
")
    log "  → KNEE_IO_WORKERS = ${KNEE_IO_WORKERS}"
}

compile_all() {
    [[ -f "$IBENCH_CPU_SRC"   ]] || die "iBench_cpu.cc not found:   $IBENCH_CPU_SRC"
    [[ -f "$IBENCH_IO_SRC"    ]] || die "iBench_io.cc not found:    $IBENCH_IO_SRC"
    [[ -f "$SPLIT_BENCH_SRC"  ]] || die "split_bench.cc not found:  $SPLIT_BENCH_SRC"

    log "Compiling iBench_cpu ..."
    IBENCH_CPU_BIN=$(mktemp /tmp/sim_cpu_XXXXX)
    g++ -O2 -pthread -o "$IBENCH_CPU_BIN" "$IBENCH_CPU_SRC" -lrt \
        || die "iBench_cpu compile failed"

    log "Compiling iBench_io ..."
    IBENCH_IO_BIN=$(mktemp /tmp/sim_io_XXXXX)
    g++ -O2 -pthread -o "$IBENCH_IO_BIN" "$IBENCH_IO_SRC" \
        || die "iBench_io compile failed"

    log "Compiling split_bench ..."
    SPLIT_BENCH_BIN=$(mktemp /tmp/sim_split_XXXXX)
    g++ -O2 -pthread -o "$SPLIT_BENCH_BIN" "$SPLIT_BENCH_SRC" -lrt \
        || die "split_bench compile failed"
}

# ── Per-second system monitor ─────────────────────────────────────────────────

setup_monitor() {
    MONITOR_SCRIPT=$(mktemp /tmp/sim_monitor_XXXXX.py)
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]

def read_disk(dev):
    if not dev: return 0, 0
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
    w.writerow(['timestamp_s', 'cpu_busy_pct', 'cpu_iowait_pct',
                'disk_read_mbs', 'disk_write_mbs'])
    fh.flush()
    while True:
        time.sleep(1)
        now     = time.time()
        cur_cpu = read_cpu()
        cur_rd, cur_wr = read_disk(disk_device)

        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        busy_pct   = 100.0 * (total - delta[3]) / total
        iowait_pct = 100.0 * delta[4] / total
        dt = (now - prev_t) or 1
        rd_mbs = (cur_rd - prev_rd) * 512 / 1024 / 1024 / dt
        wr_mbs = (cur_wr - prev_wr) * 512 / 1024 / 1024 / dt

        w.writerow([int(now), f'{busy_pct:.2f}', f'{iowait_pct:.2f}',
                    f'{rd_mbs:.3f}', f'{wr_mbs:.3f}'])
        fh.flush()
        prev_cpu = cur_cpu
        prev_rd, prev_wr = cur_rd, cur_wr
        prev_t = now
PYMON
}

# ── Plotter ───────────────────────────────────────────────────────────────────

run_plotter() {
    command -v python3 >/dev/null 2>&1 || { log "python3 not found — skipping plot"; return; }
    python3 -c "import matplotlib, pandas" 2>/dev/null \
        || { log "matplotlib/pandas not installed — skipping plot"; return; }

    local results_csv="$1" sys_csv="$2" out_png="$3"

    python3 - "$results_csv" "$sys_csv" "$out_png" \
              "$KNEE_CPU_CORES" "$KNEE_IO_WORKERS" << 'PYPLOT'
import sys
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

results_csv    = sys.argv[1]
sys_csv        = sys.argv[2]
out_png        = sys.argv[3]
knee_cpu_cores = int(sys.argv[4])
knee_io_workers= int(sys.argv[5])

df  = pd.read_csv(results_csv)
# Drop window-0 (0 workers, 0 splits) from the main curve but keep for axis
active = df[df['workers'] > 0].copy()

# Detect plateau: first worker count where marginal gain < 5%
plateau_w = None
rates = active['splits_per_sec'].values
for i in range(1, len(rates)):
    gain = (rates[i] - rates[i-1]) / max(rates[i-1], 1)
    if gain < 0.05:
        plateau_w = int(active['workers'].values[i-1])
        break

try:
    sys_df = pd.read_csv(sys_csv)
    sys_df['elapsed_s'] = sys_df['timestamp_s'] - sys_df['timestamp_s'].iloc[0]
    has_sys = True
except Exception:
    has_sys = False

plt.rcParams.update({
    'figure.dpi': 150, 'font.size': 11,
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.grid': True, 'grid.alpha': 0.35,
})

n_panels = 3 if has_sys else 1
fig = plt.figure(figsize=(10, 4 * n_panels))
gs  = gridspec.GridSpec(n_panels, 1, hspace=0.55)
ax_split = fig.add_subplot(gs[0])

# ── Panel 1: splits/s vs workers ────────────────────────────────────────────
ax_split.plot(active['workers'], active['splits_per_sec'],
              'o-', color='steelblue', linewidth=2, markersize=7,
              label='splits/s (measured)')
if plateau_w is not None:
    peak_rate = active.loc[active['workers'] == plateau_w, 'splits_per_sec'].values
    if len(peak_rate):
        ax_split.axhline(peak_rate[0], color='crimson', linestyle='--', alpha=0.6,
                         label=f'Plateau at {plateau_w} workers ({peak_rate[0]:,.0f} splits/s)')
        ax_split.axvline(plateau_w + 0.5, color='purple', linestyle=':', alpha=0.7,
                         label='Slack boundary')

ax_split.set_xlabel('Split worker threads')
ax_split.set_ylabel('Splits / second')
ax_split.set_title(
    f'In-memory split throughput under simulated knee load\n'
    f'(background: {knee_cpu_cores} CPU cores + {knee_io_workers} I/O worker(s))',
    fontsize=11)
ax_split.legend(fontsize=9)
ax_split.set_xticks(active['workers'])

if has_sys and n_panels >= 2:
    # ── Panel 2: CPU busy % over time ───────────────────────────────────────
    ax_cpu = fig.add_subplot(gs[1])
    ax_cpu.plot(sys_df['elapsed_s'], sys_df['cpu_busy_pct'],
                color='steelblue', linewidth=1.2, label='cpu_busy %')
    ax_cpu.plot(sys_df['elapsed_s'], sys_df['cpu_iowait_pct'],
                color='darkorange', linewidth=1.2, linestyle='--', label='iowait %')
    ax_cpu.set_ylim(0, 105)
    ax_cpu.set_ylabel('CPU %')
    ax_cpu.set_title('System CPU utilization over time')
    ax_cpu.legend(fontsize=9)

if has_sys and n_panels >= 3:
    # ── Panel 3: Disk I/O over time ─────────────────────────────────────────
    ax_disk = fig.add_subplot(gs[2])
    ax_disk.plot(sys_df['elapsed_s'], sys_df['disk_read_mbs'],
                 color='saddlebrown', linewidth=1.2, label='disk read MB/s')
    ax_disk.plot(sys_df['elapsed_s'], sys_df['disk_write_mbs'],
                 color='darkorange', linewidth=1.2, linestyle='--', label='disk write MB/s')
    ax_disk.set_xlabel('Elapsed time (s)')
    ax_disk.set_ylabel('Disk MB/s')
    ax_disk.set_title('Disk I/O over time (background iBench_io)')
    ax_disk.legend(fontsize=9)

plt.savefig(out_png, bbox_inches='tight')
plt.close()
print(f"Plot saved: {out_png}")
PYPLOT
}

# ── Main sweep ────────────────────────────────────────────────────────────────

run_sweep() {
    mkdir -p "$OUTPUT_DIR"
    local results_csv="$OUTPUT_DIR/results.csv"
    local sys_csv="$OUTPUT_DIR/system.csv"
    local plot_png="$OUTPUT_DIR/plot.png"

    # Auto-detect disk device for the system monitor
    local disk_device=""
    local devfile devname
    devfile=$(df "$IO_DIR" 2>/dev/null | awk 'NR==2{print $1}')
    devname="${devfile##*/}"
    for c in "$devname" \
              "$(echo "$devname" | sed -E 's/p[0-9]+$//')" \
              "$(echo "$devname" | sed -E 's/[0-9]+$//')"; do
        if [[ -n "$c" ]] && \
           awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$c"; then
            disk_device="$c"; break
        fi
    done
    [[ -n "$disk_device" ]] \
        && log "  Disk device detected: $disk_device" \
        || log "  WARNING: disk device not detected — disk I/O stats will be zero"

    # ── Start persistent background: iBench_cpu ──────────────────────────────
    # Simulates YCSB CPU footprint: KNEE_CPU_CORES busy-loop threads consuming
    # that many cores for the entire experiment duration.
    sep
    log "Starting background CPU load: ${KNEE_CPU_CORES} iBench_cpu thread(s) × ${TOTAL_SECS}s"
    "$IBENCH_CPU_BIN" "$TOTAL_SECS" "$KNEE_CPU_CORES" 0 512 \
        > "$OUTPUT_DIR/ibench_cpu.log" 2>&1 &
    IBENCH_CPU_PID=$!
    log "  iBench_cpu pid=$IBENCH_CPU_PID"

    # ── Start persistent background: iBench_io ───────────────────────────────
    # Simulates RocksDB I/O: KNEE_IO_WORKERS units each doing sequential
    # O_DIRECT writes + random O_DIRECT reads on the same disk as RocksDB.
    # Uses normal (single-pass) mode; read-file init is included in the 3s
    # settle period below (512 MiB per worker initializes in ~1-2s at disk speed).
    log "Starting background I/O load: ${KNEE_IO_WORKERS} iBench_io worker(s) × ${TOTAL_SECS}s"
    log "  IO files in: $IO_DIR  (${IO_FILE_MB} MiB per worker)"
    "$IBENCH_IO_BIN" "$TOTAL_SECS" "$KNEE_IO_WORKERS" 2097152 "$IO_DIR" "$IO_FILE_MB" \
        > "$OUTPUT_DIR/ibench_io.log" 2>&1 &
    IBENCH_IO_PID=$!
    log "  iBench_io  pid=$IBENCH_IO_PID"

    # ── Start system monitor ─────────────────────────────────────────────────
    python3 "$MONITOR_SCRIPT" "$sys_csv" "$disk_device" &
    MONITOR_PID=$!

    # ── Settle ───────────────────────────────────────────────────────────────
    # Allow iBench_io to finish read-file initialisation and both background
    # processes to reach steady-state I/O before we begin measuring splits.
    log "Settling for 3s ..."
    sleep 3

    # ── Sweep split workers ───────────────────────────────────────────────────
    echo "workers,splits_total,splits_per_sec" > "$results_csv"

    sep
    log "Beginning sweep: 0 to ${MAX_SPLIT_WORKERS} split workers, ${XPUT_WINDOW}s per window"
    log ""

    for (( n = 0; n <= MAX_SPLIT_WORKERS; n++ )); do

        # Verify background processes are still alive
        if ! kill -0 "$IBENCH_CPU_PID" 2>/dev/null; then
            log "WARNING: iBench_cpu exited early — stopping sweep at window $n"
            break
        fi
        if ! kill -0 "$IBENCH_IO_PID" 2>/dev/null; then
            log "WARNING: iBench_io exited early — stopping sweep at window $n"
            break
        fi

        if (( n == 0 )); then
            log "Window 0: background-only baseline (no split workers), ${XPUT_WINDOW}s"
            sleep "$XPUT_WINDOW"
            echo "0,0,0" >> "$results_csv"
            log "  → 0 splits/s (baseline)"
            continue
        fi

        log "Window $n: $n split worker(s) × ${XPUT_WINDOW}s ..."

        local tmp_out
        tmp_out=$(mktemp /tmp/sim_split_out_XXXXX.txt)

        # Run split_bench for exactly XPUT_WINDOW seconds with n workers.
        # Captures stdout ("split_bench: workers=N  splits=X  rate=Y splits/s").
        "$SPLIT_BENCH_BIN" "$XPUT_WINDOW" "$n" \
            > "$tmp_out" 2>> "$OUTPUT_DIR/split_bench.log"

        # Parse summary line
        local splits rate
        splits=$(grep -oP 'splits=\K[0-9]+' "$tmp_out" | head -1 || echo "0")
        rate=$(  grep -oP 'rate=\K[0-9.]+' "$tmp_out"  | head -1 || echo "0")
        rm -f "$tmp_out"

        log "  → ${n} workers: ${rate} splits/s  (${splits} total in ${XPUT_WINDOW}s)"
        echo "$n,$splits,$rate" >> "$results_csv"
    done

    # ── Stop background ───────────────────────────────────────────────────────
    sep
    log "Stopping background processes ..."
    for pid_var in IBENCH_CPU_PID IBENCH_IO_PID MONITOR_PID; do
        eval "pid=\${${pid_var}:-}"
        if [[ -n "$pid" ]]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    IBENCH_CPU_PID=""; IBENCH_IO_PID=""; MONITOR_PID=""

    # ── Print summary ─────────────────────────────────────────────────────────
    sep
    log "Results:"
    log ""
    column -t -s',' "$results_csv" | while IFS= read -r line; do log "  $line"; done
    log ""

    # ── Plot ──────────────────────────────────────────────────────────────────
    run_plotter "$results_csv" "$sys_csv" "$plot_png"

    sep
    log "Done."
    log "  Results CSV : $results_csv"
    log "  System CSV  : $sys_csv"
    log "  Plot        : $plot_png"
    log ""
    log "Interpreting the results:"
    log "  - splits/s should scale linearly while CPU slack is available"
    log "  - the plateau marks the maximum sustainable transform rate"
    log "    under the simulated knee-loaded system"
    log "  - plateau_rate / plateau_workers = splits/s per core in this workload"
}

# ── Entry point ───────────────────────────────────────────────────────────────

main() {
    sep
    log "sim_bottleneck_sweep.sh"
    log "  Simulated knee load:"
    log "    CPU: ${KNEE_CPU_CORES} iBench_cpu busy-loop thread(s)"
    if [[ -n "${KNEE_IO_MB_S:-}" ]]; then
        log "    I/O: target ${KNEE_IO_MB_S} MB/s → will calibrate worker count"
    else
        log "    I/O: ${KNEE_IO_WORKERS} iBench_io worker(s) (set directly)"
    fi
    log "         dir: ${IO_DIR}  file: ${IO_FILE_MB} MiB/worker"
    log "  Split sweep: 1..${MAX_SPLIT_WORKERS} workers × ${XPUT_WINDOW}s per window"
    log "  Record:      2048 B (16 fields × 128 B), split 8+8 fields, in-memory"
    log "  Output:      ${OUTPUT_DIR}"
    sep

    compile_all
    mkdir -p "$OUTPUT_DIR"

    # Resolve KNEE_IO_WORKERS: prefer direct setting, else calibrate from MB/s
    if [[ -n "${KNEE_IO_WORKERS:-}" ]]; then
        log "  I/O workers: ${KNEE_IO_WORKERS} (set directly)"
    elif [[ -n "${KNEE_IO_MB_S:-}" ]]; then
        log "  Target knee I/O: ${KNEE_IO_MB_S} MB/s — running calibration ..."
        calibrate_io_workers
    else
        die "Set either KNEE_IO_MB_S (preferred) or KNEE_IO_WORKERS.\n" \
            "  KNEE_IO_MB_S: disk_read_mb/s + disk_write_mb/s from saturation_sweep summary.csv\n" \
            "  KNEE_IO_WORKERS: number of iBench_io workers (if already known)"
    fi

    setup_monitor
    run_sweep
}

main "$@"
