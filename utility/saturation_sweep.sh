#!/usr/bin/env bash
# =============================================================================
# saturation_sweep.sh — RocksDB write saturation characterization
#
# Story: at a fixed value size (default 4 KB), sweep client write threads and
# show write throughput, disk I/O, and CPU utilization on the same X-axis —
# all with standard deviation error bars.  The knee in throughput identifies
# the saturation point; the IO and CPU traces reveal what the bottleneck is.
#
# Run from the project BUILD directory:
#   cd /path/to/build && bash ../utility/saturation_sweep.sh
#
# ── Output ───────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   run_t<N>/
#     ycsb_t<N>.log          — raw YCSB output (throughput mean+stddev inside)
#     system_t<N>.csv        — per-second CPU/disk samples
#   plots/
#     saturation_curve.png   — THE plot: 3-panel, same X, error bars + knee
#     saturation_summary.csv — machine-readable summary
#
# ── Key parsing note ─────────────────────────────────────────────────────────
# YCSB prints:  throughput mean:<mean>  stddev: <stddev>
# after a hard-coded 60 s warmup skip inside runXput().
# The system monitor collects per-second CPU/disk; we skip the same 60 s here.
#
# ── Configuration ─────────────────────────────────────────────────────────────
# =============================================================================

set -euo pipefail

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
DB_BASE_PATH="${DB_BASE_PATH:-/holly/sat_exp_db}"
OUTPUT_DIR="${OUTPUT_DIR:-./sat_results_$(date +%Y%m%d_%H%M%S)}"
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── Experiment knobs ──────────────────────────────────────────────────────────

# Value layout for saturation characterization.
# 16 fields × 256 B = 4096 B total per record (+ 16 B key).
# Matches Mycelium's chunk granularity so baseline and Mycelium runs are
# directly comparable: baseline issues 1 Put of ~4096 B; Mycelium will
# issue 16 Puts of 256 B each for the same logical record.
FIELD_LENGTH="${FIELD_LENGTH:-256}"
FIELD_COUNT="${FIELD_COUNT:-16}"

# Thread counts to sweep.  The script continues past the detected knee so you
# can confirm the plateau/drop.  Add more high-thread entries if needed.
THREAD_COUNTS="${THREAD_COUNTS:-1 2 4 8 12 16 20 24 32 48}"

# Total experiment duration per thread count (seconds).
# YCSB skips the first 60 s as warmup; the remaining window is used for stats.
# Keep this ≥ 90 s for meaningful stddev.  120 s gives a 60 s steady window.
RUNTIME_SECS="${RUNTIME_SECS:-120}"

# Number of records to pre-load.
RECORD_COUNT="${RECORD_COUNT:-5000000}"

# Block device for disk I/O monitoring.  Find yours with: lsblk / df -h <dbpath>
DISK_DEVICE="${DISK_DEVICE:-nvme0c0n1}"

# Warmup seconds to skip when computing CPU/disk stats from the system CSV.
# Must match YCSB's internal skip (hardcoded 60 s in runXput).
WARMUP_SKIP_S="${WARMUP_SKIP_S:-60}"

# =============================================================================
# Internals
# =============================================================================

MONITOR_SCRIPT=""
PLOTTER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""

log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    [[ -n "${MONITOR_SCRIPT:-}" ]] && rm -f "$MONITOR_SCRIPT"
    [[ -n "${PLOTTER_SCRIPT:-}" ]]  && rm -f "$PLOTTER_SCRIPT"
    [[ -n "${TMPSPEC:-}" ]]         && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]] || die "ycsb_test binary not found at '$BINARY'."
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    python3 -c "import matplotlib, pandas, numpy" 2>/dev/null || {
        log "Installing required Python packages..."
        pip3 install --user matplotlib pandas numpy -q \
            || sudo apt-get install -y python3-matplotlib python3-pandas python3-numpy -q
    }
    [[ -d "$WORKLOAD_DIR" ]] || die "Workload dir not found: $WORKLOAD_DIR"
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: device '$DISK_DEVICE' not found in /proc/diskstats — disk I/O will be zero."
        log "  Available: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
    fi
    (( RUNTIME_SECS > WARMUP_SKIP_S )) || \
        die "RUNTIME_SECS ($RUNTIME_SECS) must be > WARMUP_SKIP_S ($WARMUP_SKIP_S)"
}

# ── Embedded per-second system monitor ───────────────────────────────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp /tmp/sat_monitor_XXXXX.py)
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
"""Per-second CPU and disk I/O monitor. Writes to stdout-flushed CSV."""
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]   # user nice sys idle iowait irq softirq steal

def read_disk(dev):
    if not dev:
        return 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                return int(p[5]), int(p[9])   # sectors_read, sectors_written
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
        user_pct   = 100.0 * delta[0] / total
        sys_pct    = 100.0 * delta[2] / total
        iowait_pct = 100.0 * delta[4] / total
        idle_pct   = 100.0 * delta[3] / total

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

start_monitor() { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor()  {
    [[ -n "${MONITOR_PID:-}" ]] && { kill "$MONITOR_PID" 2>/dev/null || true; wait "$MONITOR_PID" 2>/dev/null || true; MONITOR_PID=""; }
}

# ── Workload spec ─────────────────────────────────────────────────────────────

create_spec() {
    TMPSPEC=$(mktemp /tmp/sat_spec_XXXXX.spec)
    cat > "$TMPSPEC" << EOF
keylength=16
fieldcount=${FIELD_COUNT}
fieldlength=${FIELD_LENGTH}
recordcount=${RECORD_COUNT}
operationcount=${RECORD_COUNT}
workload=com.yahoo.ycsb.workloads.CoreWorkload
readallfields=true
readproportion=0
updateproportion=1
scanproportion=0
insertproportion=0
requestdistribution=zipfian
EOF
    for kv in "$@"; do echo "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

# ── Load phase ────────────────────────────────────────────────────────────────

load_db() {
    local dbpath="$1" spec="$2"
    log "Loading $RECORD_COUNT records (fieldlength=${FIELD_LENGTH}) into $dbpath ..."
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap true -threads 8 \
        -load true -run false -throughput false \
        -throughputtype 2 -runtime 0 \
        -levels 6 -table baseline \
        2>&1 | tee "$(dirname "$dbpath")/load.log"
    log "Load complete."
}

# ── Single thread-count run ───────────────────────────────────────────────────

run_one() {
    local threads="$1" run_dir="$2" dbpath="$3"

    local sys_csv="$run_dir/system_t${threads}.csv"
    local log_file="$run_dir/ycsb_t${threads}.log"

    local cmp_csv="$run_dir/compaction_metrics.csv"
    local spec
    # Pass metrics_output so CompactionMetricsListener writes per-event stats
    # (stall_micros_cumulative, write_amp, pending_compaction_bytes, …)
    spec=$(create_spec "metrics_output=${cmp_csv}")

    log "  Running $threads thread(s) for ${RUNTIME_SECS}s ..."
    start_monitor "$sys_csv"
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$threads" \
        -load false -run false -throughput true \
        -throughputtype 2 -runtime "$RUNTIME_SECS" \
        -levels 6 -table baseline \
        2>&1 | tee "$log_file"
    stop_monitor

    rm -f "$spec"; TMPSPEC=""
    log "  → log:        $log_file"
    log "  → sys:        $sys_csv"
    log "  → compaction: $cmp_csv"
}

# ── Main sweep ────────────────────────────────────────────────────────────────

run_sweep() {
    sep
    log "Saturation sweep: fieldlength=${FIELD_LENGTH} B, threads: $THREAD_COUNTS"
    log "  Runtime per point: ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"

    local sweep_dir="$OUTPUT_DIR/sweep"
    mkdir -p "$sweep_dir"

    local dbpath="$sweep_dir/rocksdb"

    # Load once; all thread-count runs share the same DB.
    local load_spec
    load_spec=$(create_spec)
    load_db "$dbpath" "$load_spec"
    rm -f "$load_spec"; TMPSPEC=""

    for threads in $THREAD_COUNTS; do
        sep
        log "Thread count: $threads"
        local run_dir="$sweep_dir/run_t${threads}"
        mkdir -p "$run_dir"
        run_one "$threads" "$run_dir" "$dbpath"
    done

    log "Sweep complete."
}

# ── Python plotter ────────────────────────────────────────────────────────────

write_plotter() {
    PLOTTER_SCRIPT=$(mktemp /tmp/sat_plotter_XXXXX.py)
    cat > "$PLOTTER_SCRIPT" << PYPLOT
#!/usr/bin/env python3
"""
saturation_plotter.py

Produces:  plots/saturation_curve.png
           plots/saturation_summary.csv

Story: write throughput vs client threads with error bars.  Annotates the
knee (max throughput).  Same X-axis panels for disk write I/O and CPU
utilization, so the reader can see which resource saturates at the knee.
"""
import sys, os, re, glob
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

OUTPUT_DIR   = sys.argv[1]
WARMUP_SKIP  = int(sys.argv[2])   # seconds to skip from system CSV
SWEEP_DIR    = os.path.join(OUTPUT_DIR, "sweep")
PLOT_DIR     = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

# ── Parse throughput from YCSB log ──────────────────────────────────────────

def parse_ycsb_log(log_path):
    """Return (mean_ops, stddev_ops) or (nan, nan) if not found."""
    try:
        text = open(log_path).read()
        m = re.search(
            r'throughput mean:\s*([\d.eE+\-]+)\s+stddev:\s*([\d.eE+\-]+)',
            text)
        if m:
            return float(m.group(1)), float(m.group(2))
    except Exception as e:
        print(f"  [skip log] {log_path}: {e}")
    return float('nan'), float('nan')

# ── Parse system CSV for CPU and disk ───────────────────────────────────────

def parse_system_csv(csv_path, warmup_skip):
    """Return dict with mean/std for cpu_active_pct, disk_read_mbs, disk_write_mbs."""
    empty = dict(cpu_active_mean=np.nan, cpu_active_std=np.nan,
                 disk_read_mean=np.nan,  disk_read_std=np.nan,
                 disk_write_mean=np.nan, disk_write_std=np.nan)
    try:
        df = pd.read_csv(csv_path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        steady = df[df['elapsed_s'] >= warmup_skip]
        if steady.empty:
            steady = df
        active = 100.0 - steady['cpu_idle_pct']
        dr     = steady['disk_read_mbs']
        dw     = steady['disk_write_mbs']
        return dict(
            cpu_active_mean  = active.mean(),
            cpu_active_std   = active.std(ddof=1),
            disk_read_mean   = dr.mean(),
            disk_read_std    = dr.std(ddof=1),
            disk_write_mean  = dw.mean(),
            disk_write_std   = dw.std(ddof=1),
        )
    except Exception as e:
        print(f"  [skip sys] {csv_path}: {e}")
        return empty

# ── Collect data ─────────────────────────────────────────────────────────────

def thread_from_path(p):
    m = re.search(r'_t(\d+)', os.path.basename(p))
    return int(m.group(1)) if m else 0

run_dirs = sorted(
    glob.glob(os.path.join(SWEEP_DIR, "run_t*")),
    key=lambda p: thread_from_path(p)
)

rows = []
for rd in run_dirs:
    t = thread_from_path(rd)
    log_path = os.path.join(rd, f"ycsb_t{t}.log")
    sys_path = os.path.join(rd, f"system_t{t}.csv")

    xput_mean, xput_std = parse_ycsb_log(log_path)
    sys_stats           = parse_system_csv(sys_path, WARMUP_SKIP)

    rows.append(dict(
        threads        = t,
        xput_mean      = xput_mean,
        xput_std       = xput_std,
        **sys_stats,
    ))

if not rows:
    print("No run directories found — nothing to plot.")
    sys.exit(0)

df = pd.DataFrame(rows).sort_values('threads').reset_index(drop=True)
df.to_csv(os.path.join(PLOT_DIR, "saturation_summary.csv"), index=False)
print("Summary CSV written.")
print(df.to_string(index=False))

# ── Detect knee ──────────────────────────────────────────────────────────────
# Knee = the thread count at peak throughput (throughput stops increasing here).
# argmax is correct: the saturation point is where adding more threads first
# stops helping, i.e. the peak — not the first point "close to" the maximum.

valid = df.dropna(subset=['xput_mean'])
if not valid.empty:
    peak_idx     = valid['xput_mean'].idxmax()
    knee_threads = int(valid.loc[peak_idx, 'threads'])
    knee_xput    = valid.loc[peak_idx, 'xput_mean']
    print(f"Knee detected at {knee_threads} threads ({knee_xput:.0f} ops/s)")
else:
    knee_threads = None

# ── Compaction diagnostics ────────────────────────────────────────────────────
# CompactionMetricsListener writes one row per compaction event.
# Key columns:
#   stall_micros_cumulative — running total of write-stall time (µs)
#   write_amp               — bytes_out / bytes_in for that compaction job
#   pending_compaction_bytes — estimated backlog at event time
#   elapsed_micros          — wall-clock duration of the compaction job
#
# We read the FINAL row of each run's CSV (end-of-experiment snapshot) and
# print a diagnostic table so you can see at a glance whether compaction
# pressure grows with thread count.

def parse_compaction_csv(csv_path):
    try:
        cdf = pd.read_csv(csv_path)
        if cdf.empty:
            return None
        last = cdf.iloc[-1]
        return dict(
            num_events        = len(cdf),
            stall_ms          = last['stall_micros_cumulative'] / 1_000,
            # stall_micros_cumulative is a wall-clock counter covering the full
            # run (including warmup), so divide by RUNTIME_SECS, not the
            # steady-state window, to avoid inflating the percentage past 100%.
            stall_pct         = (last['stall_micros_cumulative'] / 1e6)
                                / RUNTIME_SECS * 100
                                if RUNTIME_SECS > 0 else float('nan'),
            write_amp_mean    = cdf['write_amp'].mean(),
            write_amp_max     = cdf['write_amp'].max(),
            pending_bytes_max = cdf['pending_compaction_bytes'].max() / (1024**3),
            compaction_s_total= cdf['elapsed_micros'].sum() / 1e6,
        )
    except Exception as e:
        print(f"  [skip compaction csv] {csv_path}: {e}")
        return None

RUNTIME_SECS  = int(sys.argv[5])
WARMUP_SKIP   = int(sys.argv[2])   # already defined above, reuse

print()
print("── Compaction diagnostics ───────────────────────────────────────────────────")
print(f"{'threads':>8}  {'events':>6}  {'stall_ms':>10}  {'stall_%':>8}  "
      f"{'write_amp_mean':>14}  {'write_amp_max':>13}  {'pending_GB_max':>14}")
for rd in run_dirs:
    t = thread_from_path(rd)
    c = parse_compaction_csv(os.path.join(rd, f"compaction_metrics.csv"))
    if c:
        print(f"{t:>8}  {c['num_events']:>6}  {c['stall_ms']:>10.1f}  "
              f"{c['stall_pct']:>7.1f}%  {c['write_amp_mean']:>14.2f}  "
              f"{c['write_amp_max']:>13.2f}  {c['pending_bytes_max']:>14.3f}")
    else:
        print(f"{t:>8}  (no compaction data — listener may not have fired)")
print("─────────────────────────────────────────────────────────────────────────────")
print()
print("  stall_%    : fraction of the full run spent in write stall")
print("  write_amp  : SST bytes written / bytes read per compaction job")
print("  pending_GB : compaction backlog at end of run (> 0 means falling behind)")
print()

# ── Plot ─────────────────────────────────────────────────────────────────────

plt.rcParams.update({
    'figure.dpi':        150,
    'font.size':         11,
    'axes.spines.top':   False,
    'axes.spines.right': False,
    'axes.grid':         True,
    'grid.alpha':        0.35,
    'axes.labelsize':    11,
    'xtick.labelsize':   10,
    'ytick.labelsize':   10,
})

fig = plt.figure(figsize=(13, 11))
gs  = gridspec.GridSpec(3, 1, hspace=0.45)

ax_xput = fig.add_subplot(gs[0])
ax_disk = fig.add_subplot(gs[1], sharex=ax_xput)
ax_cpu  = fig.add_subplot(gs[2], sharex=ax_xput)

threads = df['threads'].values

# ── Panel 1: write throughput ─────────────────────────────────────────────────

ax_xput.errorbar(threads, df['xput_mean'], yerr=df['xput_std'],
                 fmt='o-', color='steelblue', linewidth=2, markersize=7,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Write throughput (ops/s)')
ax_xput.set_ylabel('Write throughput\n(ops / sec)')
ax_xput.set_title('Write throughput vs client threads  (± 1 σ over steady-state window)')

if knee_threads is not None:
    ax_xput.axvline(knee_threads, color='crimson', linestyle='--', alpha=0.7,
                    label=f'Knee @ {knee_threads} threads')
    ax_xput.annotate(
        f'Knee\n{knee_threads} threads',
        xy=(knee_threads, knee_xput),
        xytext=(knee_threads + max(threads) * 0.05, knee_xput * 0.88),
        fontsize=9, color='crimson',
        arrowprops=dict(arrowstyle='->', color='crimson', lw=1.4))
ax_xput.legend(fontsize=9, loc='lower right')

# ── Panel 2: disk read + write MB/s ──────────────────────────────────────────
# Both matter: compaction reads SST input files (read I/O) and writes new ones
# (write I/O).  The true bandwidth pressure is the sum of both.

ax_disk.errorbar(threads, df['disk_write_mean'], yerr=df['disk_write_std'],
                 fmt='s-', color='darkorange', linewidth=2, markersize=7,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Disk write (MB/s)')
ax_disk.errorbar(threads, df['disk_read_mean'], yerr=df['disk_read_std'],
                 fmt='D--', color='saddlebrown', linewidth=2, markersize=6,
                 capsize=5, capthick=1.5, elinewidth=1.5,
                 label='Disk read (MB/s)')
ax_disk.set_ylabel('Disk I/O\n(MB / sec)')
ax_disk.set_title('Disk read + write bandwidth vs client threads  (± 1 σ)')

if knee_threads is not None:
    ax_disk.axvline(knee_threads, color='crimson', linestyle='--', alpha=0.7)
ax_disk.legend(fontsize=9, loc='lower right')

# ── Panel 3: CPU active % ────────────────────────────────────────────────────

ax_cpu.errorbar(threads, df['cpu_active_mean'], yerr=df['cpu_active_std'],
                fmt='^-', color='forestgreen', linewidth=2, markersize=7,
                capsize=5, capthick=1.5, elinewidth=1.5,
                label='CPU active (100 − idle) %')
ax_cpu.set_ylabel('CPU utilization\n(100 − idle, %)')
ax_cpu.set_title('System CPU utilization vs client threads  (± 1 σ)')
ax_cpu.set_xlabel('Client write threads')
ax_cpu.set_ylim(0, 105)
ax_cpu.axhline(100, color='gray', linestyle=':', alpha=0.6, label='100% ceiling')

if knee_threads is not None:
    ax_cpu.axvline(knee_threads, color='crimson', linestyle='--', alpha=0.7)
ax_cpu.legend(fontsize=9, loc='lower right')

# ── Shared X-axis ticks ───────────────────────────────────────────────────────

ax_cpu.set_xticks(threads)
plt.setp(ax_xput.get_xticklabels(), visible=False)
plt.setp(ax_disk.get_xticklabels(), visible=False)

field_length = int(sys.argv[3])
field_count  = int(sys.argv[4])
total_bytes  = field_count * field_length
fig.suptitle(
    f'RocksDB write saturation characterization  '
    f'({field_count} fields × {field_length} B = {total_bytes} B / record)',
    fontsize=13, fontweight='bold', y=0.98)

out = os.path.join(PLOT_DIR, "saturation_curve.png")
plt.savefig(out, bbox_inches='tight')
plt.close()
print(f"\nPlot saved: {out}")
PYPLOT
}

run_plotter() {
    sep
    log "Generating saturation plots..."
    write_plotter
    python3 "$PLOTTER_SCRIPT" "$OUTPUT_DIR" "$WARMUP_SKIP_S" "$FIELD_LENGTH" "$FIELD_COUNT" "$RUNTIME_SECS"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    log "saturation_sweep.sh starting"
    log "  Binary:      $BINARY"
    log "  Output dir:  $OUTPUT_DIR"
    log "  Device:      $DISK_DEVICE"
    log "  Value layout: ${FIELD_COUNT} fields × ${FIELD_LENGTH} B = $((FIELD_COUNT * FIELD_LENGTH)) B/record  (+16 B key)"
    log "  Threads:     $THREAD_COUNTS"
    log "  Runtime/pt:  ${RUNTIME_SECS}s  (warmup skip: ${WARMUP_SKIP_S}s)"
    echo ""

    check_prereqs
    setup_monitor_script
    mkdir -p "$OUTPUT_DIR"

    run_sweep
    run_plotter

    sep
    log "Done."
    log "  Results: $OUTPUT_DIR"
    log "  Plot:    $OUTPUT_DIR/plots/saturation_curve.png"
    log ""
    log "  Serve locally (no scp needed):"
    log "    python3 -m http.server 8888 --directory $OUTPUT_DIR"
    log "    Then open: http://$(hostname -I | awk '{print $1}' 2>/dev/null || echo localhost):8888"
}

main "$@"
