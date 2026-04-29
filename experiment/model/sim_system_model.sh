#!/usr/bin/env bash
# =============================================================================
# sim_system_model.sh — Model a system (e.g. RocksDB+YCSB) in terms of its
#                       resource usage using iBench_cpu and iBench_io threads.
#
# Motivation:
#   Before running a split-transform sweep (sim_bottleneck_sweep.sh), or when
#   you simply want to understand how much CPU and I/O headroom a particular
#   operating point leaves, this script lets you reproduce that operating point
#   purely via iBench interference threads and measure the resulting system-
#   resource profile.
#
#   Unlike sim_bottleneck_sweep.sh, this script performs NO split-transform
#   work.  It is purely a resource-modeling tool:
#     - iBench_cpu: TARGET_CPU_CORES busy-loop threads → reproduces CPU load
#     - iBench_io:  TARGET_IO_WORKERS read+write units → reproduces I/O load
#
#   The script records per-second CPU utilization and disk bandwidth for the
#   entire RUN_SECS duration, then prints a summary and optional plot.
#
# Typical use cases:
#   1. Verify that iBench reproduces the resource footprint observed during a
#      real RocksDB/YCSB run (cross-check against saturation_sweep output).
#   2. Establish a baseline resource profile at a given operating point before
#      overlaying a split-transform sweep.
#   3. Calibrate KNEE_IO_WORKERS for a target disk bandwidth without needing
#      to run a full sweep.
#
# Usage:
#   cd /path/to/build
#   TARGET_CPU_CORES=12  TARGET_IO_MB_S=350  IO_DIR=/mnt/rocksdb-disk \
#     bash ../experiment/model/sim_system_model.sh
#
#   # Or, if you already know the I/O worker count:
#   TARGET_CPU_CORES=12  TARGET_IO_WORKERS=3  IO_DIR=/mnt/rocksdb-disk \
#     bash ../experiment/model/sim_system_model.sh
#
# Required (one of):
#   TARGET_CPU_CORES   Number of iBench_cpu busy-loop threads.
#                      Corresponds to the CPU cores consumed by the workload
#                      at the target operating point:
#                        busy_pct/100 × nproc  (from saturation_sweep summary.csv)
#
#   TARGET_IO_MB_S     Total disk bandwidth to reproduce in MB/s (read+write).
#                      The script runs a short iBench_io calibration (15s) on
#                      IO_DIR to measure single-worker bandwidth, then sets
#                        TARGET_IO_WORKERS = ceil(TARGET_IO_MB_S / single_mbs)
#                      Mutually exclusive with TARGET_IO_WORKERS.
#
#   TARGET_IO_WORKERS  Explicit number of iBench_io worker threads.
#                      Use when you already know the right count from a prior
#                      calibration run.  Mutually exclusive with TARGET_IO_MB_S.
#
# Optional:
#   IO_DIR             Directory for iBench_io temp files; MUST be on the same
#                      physical disk as the system being modeled (default: /tmp)
#   IO_FILE_MB         iBench_io file size per worker in MiB (default: 512)
#   RUN_SECS           Total duration of the model run in seconds (default: 60)
#   SETTLE_SECS        Warm-up time before recording begins (default: 3)
#   OUTPUT_DIR         Results directory (default: ./sim_model_<timestamp>)
#
# Output files:
#   <OUTPUT_DIR>/system.csv        — per-second: cpu_busy_pct, cpu_iowait_pct,
#                                    disk_read_mbs, disk_write_mbs
#   <OUTPUT_DIR>/summary.txt       — aggregate stats (mean ± stddev for each metric)
#   <OUTPUT_DIR>/ibench_cpu.log    — iBench_cpu stdout/stderr
#   <OUTPUT_DIR>/ibench_io.log     — iBench_io stdout/stderr
#   <OUTPUT_DIR>/ibench_io_calib.log — calibration output (when TARGET_IO_MB_S set)
#   <OUTPUT_DIR>/plot.png          — time-series resource plot (if matplotlib)
# =============================================================================

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────

TARGET_CPU_CORES="${TARGET_CPU_CORES:-4}"

# TARGET_IO_MB_S: total disk bandwidth (read + write) to reproduce in MB/s.
# When set, the script runs a short iBench_io calibration to derive the worker
# count.  Leave empty and set TARGET_IO_WORKERS directly if already known.
TARGET_IO_MB_S="${TARGET_IO_MB_S:-}"

# TARGET_IO_WORKERS: explicit worker count override; skips calibration.
TARGET_IO_WORKERS="${TARGET_IO_WORKERS:-}"

IO_DIR="${IO_DIR:-/tmp}"
IO_FILE_MB="${IO_FILE_MB:-512}"

RUN_SECS="${RUN_SECS:-60}"
SETTLE_SECS="${SETTLE_SECS:-3}"

OUTPUT_DIR="${OUTPUT_DIR:-./sim_model_$(date +%Y%m%d_%H%M%S)}"

# Source locations: look for iBench sources next to this script, then fall back
# to the sibling slack/ directory (where they typically live).
MODEL_DIR="$(cd "$(dirname "$0")" && pwd)"
SLACK_DIR="$(cd "$MODEL_DIR/../slack" 2>/dev/null && pwd || echo "$MODEL_DIR")"

IBENCH_CPU_SRC="${IBENCH_CPU_SRC:-$SLACK_DIR/iBench_cpu.cc}"
IBENCH_IO_SRC="${IBENCH_IO_SRC:-$SLACK_DIR/iBench_io.cc}"

# Total iBench run time: settle + measurement + small buffer so processes do
# not exit before the monitor finishes flushing the last sample.
TOTAL_SECS=$(( SETTLE_SECS + RUN_SECS + 5 ))

# =============================================================================

IBENCH_CPU_BIN=""
IBENCH_IO_BIN=""
IBENCH_CPU_PID=""
IBENCH_IO_PID=""
MONITOR_PID=""
MONITOR_SCRIPT=""

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${IBENCH_CPU_PID:-}" ]] && kill "$IBENCH_CPU_PID" 2>/dev/null || true
    [[ -n "${IBENCH_IO_PID:-}"  ]] && kill "$IBENCH_IO_PID"  2>/dev/null || true
    [[ -n "${MONITOR_PID:-}"    ]] && kill "$MONITOR_PID"    2>/dev/null || true
    [[ -n "${IBENCH_CPU_BIN:-}" ]] && rm -f "$IBENCH_CPU_BIN"
    [[ -n "${IBENCH_IO_BIN:-}"  ]] && rm -f "$IBENCH_IO_BIN"
    [[ -n "${MONITOR_SCRIPT:-}" ]] && rm -f "$MONITOR_SCRIPT"
}
trap cleanup EXIT

# ── Compile ───────────────────────────────────────────────────────────────────

compile_all() {
    [[ -f "$IBENCH_CPU_SRC" ]] || die "iBench_cpu.cc not found: $IBENCH_CPU_SRC"
    [[ -f "$IBENCH_IO_SRC"  ]] || die "iBench_io.cc not found:  $IBENCH_IO_SRC"

    log "Compiling iBench_cpu ..."
    IBENCH_CPU_BIN=$(mktemp /tmp/sysmodel_cpu_XXXXX)
    g++ -O2 -pthread -o "$IBENCH_CPU_BIN" "$IBENCH_CPU_SRC" -lrt \
        || die "iBench_cpu compile failed"

    log "Compiling iBench_io ..."
    IBENCH_IO_BIN=$(mktemp /tmp/sysmodel_io_XXXXX)
    g++ -O2 -pthread -o "$IBENCH_IO_BIN" "$IBENCH_IO_SRC" \
        || die "iBench_io compile failed"
}

# ── I/O worker calibration ────────────────────────────────────────────────────
#
# Runs iBench_io with 1 worker for CALIB_SECS seconds, parses the reported
# total MB/s, and computes how many workers are needed to reach TARGET_IO_MB_S.
# Sets TARGET_IO_WORKERS as a side-effect.

calibrate_io_workers() {
    local calib_secs=15
    local calib_file_mb=512

    log "Calibrating iBench_io: 1 worker × ${calib_secs}s on ${IO_DIR} ..."
    log "  (measuring single-worker bandwidth to derive TARGET_IO_WORKERS)"

    local calib_out
    calib_out=$(mktemp /tmp/sysmodel_calib_XXXXX.txt)

    "$IBENCH_IO_BIN" "$calib_secs" 1 2097152 "$IO_DIR" "$calib_file_mb" \
        > "$calib_out" 2>&1 || true

    local single_mbs
    single_mbs=$(grep -oP 'total:\s+\K[0-9.]+' "$calib_out" | head -1 || echo "")
    cat "$calib_out" >> "$OUTPUT_DIR/ibench_io_calib.log" 2>/dev/null || true
    rm -f "$calib_out"

    if [[ -z "$single_mbs" || "$single_mbs" == "0" ]]; then
        log "WARNING: iBench_io calibration produced no readable output."
        log "         Falling back to TARGET_IO_WORKERS=1."
        TARGET_IO_WORKERS=1
        return
    fi

    log "  Single iBench_io worker achieved: ${single_mbs} MB/s (read+write)"
    log "  Target I/O bandwidth:             ${TARGET_IO_MB_S} MB/s"

    TARGET_IO_WORKERS=$(python3 -c "
import math
target = float('${TARGET_IO_MB_S}')
single = float('${single_mbs}')
if single <= 0:
    print(1)
else:
    print(max(1, math.ceil(target / single)))
")
    log "  → TARGET_IO_WORKERS = ${TARGET_IO_WORKERS}"
}

# ── Per-second system monitor ─────────────────────────────────────────────────

setup_monitor() {
    MONITOR_SCRIPT=$(mktemp /tmp/sysmodel_monitor_XXXXX.py)
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

# ── Summary statistics ────────────────────────────────────────────────────────
#
# Reads system.csv and emits a plain-text aggregate summary (mean ± stddev)
# for each metric over the measurement window.

write_summary() {
    local sys_csv="$1" summary_txt="$2"
    command -v python3 >/dev/null 2>&1 || { log "python3 not found — skipping summary"; return; }
    python3 - "$sys_csv" "$summary_txt" \
              "$TARGET_CPU_CORES" "$TARGET_IO_WORKERS" \
              "$RUN_SECS" "$SETTLE_SECS" << 'PYSUMMARY'
import sys, csv, math

sys_csv       = sys.argv[1]
summary_txt   = sys.argv[2]
cpu_cores     = sys.argv[3]
io_workers    = sys.argv[4]
run_secs      = int(sys.argv[5])
settle_secs   = int(sys.argv[6])

rows = []
with open(sys_csv) as f:
    reader = csv.DictReader(f)
    for r in reader:
        rows.append(r)

if not rows:
    print("No data in system.csv")
    sys.exit(0)

# Skip the first settle_secs samples (warm-up period)
rows = rows[settle_secs:] if len(rows) > settle_secs else rows

def stats(vals):
    n = len(vals)
    if n == 0:
        return 0.0, 0.0
    mean = sum(vals) / n
    var  = sum((v - mean) ** 2 for v in vals) / n
    return mean, math.sqrt(var)

cpu_busy   = [float(r['cpu_busy_pct'])   for r in rows]
cpu_iowait = [float(r['cpu_iowait_pct']) for r in rows]
rd_mbs     = [float(r['disk_read_mbs'])  for r in rows]
wr_mbs     = [float(r['disk_write_mbs']) for r in rows]
total_mbs  = [r + w for r, w in zip(rd_mbs, wr_mbs)]

lines = [
    "=" * 60,
    "sim_system_model.sh — Resource Usage Summary",
    "=" * 60,
    f"  iBench_cpu threads : {cpu_cores}",
    f"  iBench_io workers  : {io_workers}",
    f"  Measurement window : {len(rows)} samples (after {settle_secs}s settle)",
    "",
    f"  {'Metric':<28} {'Mean':>10}  {'Std':>10}",
    f"  {'-'*28}  {'-'*10}  {'-'*10}",
]

def fmt(mean, std, unit):
    return f"  {'':<28}".replace(' '*28, '') + \
           f"  {mean:>10.2f}  {std:>10.2f}  {unit}"

for label, vals, unit in [
    ("cpu_busy_pct",        cpu_busy,   "%"),
    ("cpu_iowait_pct",      cpu_iowait, "%"),
    ("disk_read_mbs",       rd_mbs,     "MB/s"),
    ("disk_write_mbs",      wr_mbs,     "MB/s"),
    ("disk_total_mbs",      total_mbs,  "MB/s"),
]:
    mean, std = stats(vals)
    lines.append(f"  {label:<28} {mean:>10.2f}  {std:>10.2f}  {unit}")

lines += ["=" * 60, ""]

text = "\n".join(lines)
print(text)
with open(summary_txt, 'w') as f:
    f.write(text + "\n")
PYSUMMARY
}

# ── Plotter ───────────────────────────────────────────────────────────────────

run_plotter() {
    command -v python3 >/dev/null 2>&1 || { log "python3 not found — skipping plot"; return; }
    python3 -c "import matplotlib, pandas" 2>/dev/null \
        || { log "matplotlib/pandas not installed — skipping plot"; return; }

    local sys_csv="$1" out_png="$2"

    python3 - "$sys_csv" "$out_png" \
              "$TARGET_CPU_CORES" "$TARGET_IO_WORKERS" \
              "$SETTLE_SECS" << 'PYPLOT'
import sys
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

sys_csv     = sys.argv[1]
out_png     = sys.argv[2]
cpu_cores   = int(sys.argv[3])
io_workers  = int(sys.argv[4])
settle_secs = int(sys.argv[5])

try:
    df = pd.read_csv(sys_csv)
    df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
    df['disk_total_mbs'] = df['disk_read_mbs'] + df['disk_write_mbs']
except Exception as e:
    print(f"Could not load {sys_csv}: {e}")
    sys.exit(0)

# Drop warm-up samples from the measurement window visualisation
meas = df[df['elapsed_s'] >= settle_secs].copy()

plt.rcParams.update({
    'figure.dpi': 150, 'font.size': 11,
    'axes.spines.top': False, 'axes.spines.right': False,
    'axes.grid': True, 'grid.alpha': 0.35,
})

fig = plt.figure(figsize=(10, 8))
gs  = gridspec.GridSpec(2, 1, hspace=0.50)

title_suffix = (f"{cpu_cores} CPU thread(s)  ·  {io_workers} I/O worker(s)")

# ── Panel 1: CPU utilization ─────────────────────────────────────────────────
ax_cpu = fig.add_subplot(gs[0])
ax_cpu.plot(df['elapsed_s'], df['cpu_busy_pct'],
            color='steelblue', linewidth=1.4, label='cpu_busy %')
ax_cpu.plot(df['elapsed_s'], df['cpu_iowait_pct'],
            color='darkorange', linewidth=1.4, linestyle='--', label='iowait %')
if len(meas):
    ax_cpu.axhline(meas['cpu_busy_pct'].mean(), color='steelblue',
                   linestyle=':', alpha=0.7,
                   label=f"mean busy = {meas['cpu_busy_pct'].mean():.1f}%")
ax_cpu.axvline(settle_secs, color='gray', linestyle=':', alpha=0.5,
               label=f'settle ({settle_secs}s)')
ax_cpu.set_ylim(0, 105)
ax_cpu.set_ylabel('CPU utilization (%)')
ax_cpu.set_title(f'CPU utilization — simulated system load\n({title_suffix})',
                 fontsize=11)
ax_cpu.legend(fontsize=9)

# ── Panel 2: Disk I/O ────────────────────────────────────────────────────────
ax_disk = fig.add_subplot(gs[1])
ax_disk.plot(df['elapsed_s'], df['disk_read_mbs'],
             color='saddlebrown', linewidth=1.4, label='read MB/s')
ax_disk.plot(df['elapsed_s'], df['disk_write_mbs'],
             color='darkorange', linewidth=1.4, linestyle='--', label='write MB/s')
ax_disk.plot(df['elapsed_s'], df['disk_total_mbs'],
             color='gray', linewidth=1.0, linestyle=':', alpha=0.7,
             label='total MB/s')
if len(meas):
    ax_disk.axhline(meas['disk_total_mbs'].mean(), color='gray',
                    linestyle='--', alpha=0.6,
                    label=f"mean total = {meas['disk_total_mbs'].mean():.1f} MB/s")
ax_disk.axvline(settle_secs, color='gray', linestyle=':', alpha=0.5,
                label=f'settle ({settle_secs}s)')
ax_disk.set_xlabel('Elapsed time (s)')
ax_disk.set_ylabel('Disk bandwidth (MB/s)')
ax_disk.set_title(f'Disk I/O — simulated system load\n({title_suffix})',
                  fontsize=11)
ax_disk.legend(fontsize=9)

plt.savefig(out_png, bbox_inches='tight')
plt.close()
print(f"Plot saved: {out_png}")
PYPLOT
}

# ── Main run ──────────────────────────────────────────────────────────────────

run_model() {
    mkdir -p "$OUTPUT_DIR"
    local sys_csv="$OUTPUT_DIR/system.csv"
    local summary_txt="$OUTPUT_DIR/summary.txt"
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

    # ── Start iBench_cpu ─────────────────────────────────────────────────────
    sep
    log "Starting CPU load: ${TARGET_CPU_CORES} iBench_cpu thread(s) × ${TOTAL_SECS}s"
    "$IBENCH_CPU_BIN" "$TOTAL_SECS" "$TARGET_CPU_CORES" 0 512 \
        > "$OUTPUT_DIR/ibench_cpu.log" 2>&1 &
    IBENCH_CPU_PID=$!
    log "  iBench_cpu pid=$IBENCH_CPU_PID"

    # ── Start iBench_io ──────────────────────────────────────────────────────
    log "Starting I/O load: ${TARGET_IO_WORKERS} iBench_io worker(s) × ${TOTAL_SECS}s"
    log "  IO files in: $IO_DIR  (${IO_FILE_MB} MiB per worker)"
    "$IBENCH_IO_BIN" "$TOTAL_SECS" "$TARGET_IO_WORKERS" 2097152 "$IO_DIR" "$IO_FILE_MB" \
        > "$OUTPUT_DIR/ibench_io.log" 2>&1 &
    IBENCH_IO_PID=$!
    log "  iBench_io  pid=$IBENCH_IO_PID"

    # ── Start system monitor ─────────────────────────────────────────────────
    python3 "$MONITOR_SCRIPT" "$sys_csv" "$disk_device" &
    MONITOR_PID=$!

    # ── Settle ───────────────────────────────────────────────────────────────
    log "Settling for ${SETTLE_SECS}s (iBench_io file init + process warm-up) ..."
    sleep "$SETTLE_SECS"

    # ── Measure ──────────────────────────────────────────────────────────────
    sep
    log "Measuring system resource usage for ${RUN_SECS}s ..."
    log "  (monitor writing to $sys_csv)"

    local elapsed=0
    while (( elapsed < RUN_SECS )); do
        # Verify background processes are still alive
        if ! kill -0 "$IBENCH_CPU_PID" 2>/dev/null; then
            log "WARNING: iBench_cpu exited early at ${elapsed}s — stopping measurement"
            break
        fi
        if ! kill -0 "$IBENCH_IO_PID" 2>/dev/null; then
            log "WARNING: iBench_io exited early at ${elapsed}s — stopping measurement"
            break
        fi
        sleep 5
        elapsed=$(( elapsed + 5 ))
        log "  ${elapsed}/${RUN_SECS}s elapsed ..."
    done

    # ── Stop background processes ─────────────────────────────────────────────
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

    # ── Summary & plot ────────────────────────────────────────────────────────
    sep
    log "Writing summary ..."
    write_summary "$sys_csv" "$summary_txt"

    run_plotter "$sys_csv" "$plot_png"

    sep
    log "Done."
    log "  System CSV  : $sys_csv"
    log "  Summary     : $summary_txt"
    log "  Plot        : $plot_png"
    log ""
    log "Interpreting the results:"
    log "  - cpu_busy_pct should match the observed workload CPU utilization"
    log "  - disk_total_mbs should match the observed workload I/O bandwidth"
    log "  - If the numbers don't match, adjust TARGET_CPU_CORES / TARGET_IO_WORKERS"
    log "    and re-run to dial in the model before running sim_bottleneck_sweep.sh"
}

# ── Entry point ───────────────────────────────────────────────────────────────

main() {
    sep
    log "sim_system_model.sh — iBench-based system resource model"
    log "  CPU load   : ${TARGET_CPU_CORES} iBench_cpu busy-loop thread(s)"
    if [[ -n "${TARGET_IO_MB_S:-}" ]]; then
        log "  I/O load   : target ${TARGET_IO_MB_S} MB/s → will calibrate worker count"
    else
        log "  I/O load   : ${TARGET_IO_WORKERS} iBench_io worker(s) (set directly)"
    fi
    log "               dir: ${IO_DIR}  file: ${IO_FILE_MB} MiB/worker"
    log "  Duration   : ${SETTLE_SECS}s settle + ${RUN_SECS}s measurement"
    log "  Output     : ${OUTPUT_DIR}"
    sep

    compile_all
    mkdir -p "$OUTPUT_DIR"

    # Resolve TARGET_IO_WORKERS: prefer direct setting, else calibrate from MB/s
    if [[ -n "${TARGET_IO_WORKERS:-}" ]]; then
        log "  I/O workers: ${TARGET_IO_WORKERS} (set directly)"
    elif [[ -n "${TARGET_IO_MB_S:-}" ]]; then
        log "  Target I/O : ${TARGET_IO_MB_S} MB/s — running calibration ..."
        calibrate_io_workers
    else
        die "Set either TARGET_IO_MB_S or TARGET_IO_WORKERS.\n" \
            "  TARGET_IO_MB_S     : disk_read_mb/s + disk_write_mb/s from the\n" \
            "                       target operating point (e.g. saturation_sweep summary.csv)\n" \
            "  TARGET_IO_WORKERS  : number of iBench_io workers (if already known)"
    fi

    setup_monitor
    run_model
}

main "$@"
