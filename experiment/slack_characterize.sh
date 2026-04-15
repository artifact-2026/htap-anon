#!/usr/bin/env bash
# =============================================================================
# slack_characterize.sh — Step 1: CPU slack characterization experiments
#
# Runs experiments E1, E3, and E5, captures per-compaction CSV + system
# CPU/IO metrics, then generates PNG plots via an embedded Python script.
#
# Must be run from the project BUILD directory (where ycsb_test lives):
#   cd /path/to/build && bash ../utility/slack_characterize.sh
#
# ── Viewing plots without scping to Mac ──────────────────────────────────────
# After the script finishes it will print a command like:
#   python3 -m http.server 8888 --directory /path/to/output
# Run that on the Linux box, then open http://<server-ip>:8888 in your
# Mac browser — no scp needed.  Alternatively, if you have X11 forwarding
# (ssh -X), run: eog /path/to/output/plots/
#
# ── Experiment summary ───────────────────────────────────────────────────────
# E1  Write rate sweep        Vary client thread count to vary write rate.
#                             Produces the cpu_slack vs write_rate curve.
# E3  Breakdown point         Ramp threads until write stalls appear.
#                             Quantifies the saturation boundary.
# E5  Value size sweep        Vary record size at a fixed write rate.
#                             Input to the mRoutine cost model.
#
# E2 (hardware comparison) and E4 (compaction style) require hardware or
# code changes outside this script; see the STUB sections below.
#
# ── Configuration — edit this section ────────────────────────────────────────
# =============================================================================

set -euo pipefail

# Path to the compiled ycsb_test binary (default: look in current directory)
BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"

# Where to write RocksDB data files during experiments.
# Use a path on the storage device you want to characterize (NVMe or SATA).
DB_BASE_PATH="${DB_BASE_PATH:-/holly/slack_exp_db}"

# All CSVs, logs, and plots go here.
OUTPUT_DIR="${OUTPUT_DIR:-./slack_results_$(date +%Y%m%d_%H%M%S)}"

# Source tree root (used to find workload spec files)
SRC_ROOT="${SRC_ROOT:-$(dirname "$0")/../src}"
WORKLOAD_DIR="$SRC_ROOT/test/ycsb/workloads"

# ── Experiment knobs ──────────────────────────────────────────────────────────

# E1 / E3: number of write client threads to sweep.
# More threads → higher write rate.  Adjust to match your hardware.
THREAD_COUNTS="${THREAD_COUNTS:-1 2 4 8 16 32}"

# Duration of each xput window in seconds.
RUNTIME_SECS="${RUNTIME_SECS:-120}"

# Number of records pre-loaded into the DB before each experiment.
RECORD_COUNT="${RECORD_COUNT:-5000000}"

# E5: field (column) byte lengths to sweep.
# Total record size ≈ fieldlength + keylength(16) bytes.
FIELDLENGTHS="${FIELDLENGTHS:-100 512 1024 4096 16384}"

# Fixed thread count used in E5 (mid-range, not saturated).
E5_THREADS="${E5_THREADS:-8}"

# Block device name used for disk I/O monitoring (e.g. nvme0n1, sda).
# Find yours with: lsblk
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"

# Set to 0 to skip individual experiments.
RUN_E1="${RUN_E1:-1}"
RUN_E3="${RUN_E3:-1}"
RUN_E5="${RUN_E5:-1}"

# =============================================================================
# Internals — no user edits needed below this line
# =============================================================================

MONITOR_SCRIPT=""   # set in setup_tmpfiles
PLOTTER_SCRIPT=""   # set in setup_tmpfiles
TMPSPEC=""          # reused per-experiment

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
sep()  { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

# Clean up tmp files and any lingering monitor on exit
cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    [[ -n "${MONITOR_SCRIPT:-}" ]] && rm -f "$MONITOR_SCRIPT"
    [[ -n "${PLOTTER_SCRIPT:-}" ]] && rm -f "$PLOTTER_SCRIPT"
    [[ -n "${TMPSPEC:-}" ]]        && rm -f "$TMPSPEC"
}
trap cleanup EXIT

# ── Pre-flight checks ─────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]] || die "ycsb_test binary not found at '$BINARY'.
  Build the project first, or set BINARY=/path/to/ycsb_test"

    command -v python3 >/dev/null 2>&1 || die "python3 is required"

    python3 -c "import matplotlib, pandas" 2>/dev/null || {
        log "Installing required Python packages (matplotlib, pandas)..."
        # --user works on all pip versions (including pip 21 on Ubuntu 20.04).
        # --break-system-packages was only added in pip 23 and is not needed here.
        pip3 install --user matplotlib pandas -q \
            || { log "pip3 --user failed, trying apt..."; \
                 sudo apt-get install -y python3-matplotlib python3-pandas -q; }
    }

    [[ -d "$WORKLOAD_DIR" ]] || die "Workload dir not found: $WORKLOAD_DIR
  Set SRC_ROOT=/path/to/htap/src"

    # /proc/diskstats lines are: "major minor devname ...", so match field 3.
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: device '$DISK_DEVICE' not found in /proc/diskstats."
        log "  Disk I/O columns will be zero. Set DISK_DEVICE=<your device>."
        log "  Available devices: $(awk '{print $3}' /proc/diskstats | sort -u | tr '\n' ' ')"
    fi
}

# ── Embedded Python: system CPU + disk I/O monitor ───────────────────────────
# Reads /proc/stat and /proc/diskstats once per second, writes CSV deltas.

setup_tmpfiles() {
    MONITOR_SCRIPT=$(mktemp /tmp/slack_monitor_XXXXX.py)
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
"""Per-second system CPU and disk I/O monitor. Writes to CSV."""
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    with open('/proc/stat') as f:
        parts = f.readline().split()
    # user nice system idle iowait irq softirq steal
    return [int(x) for x in parts[1:9]]

def read_disk(dev):
    if not dev:
        return 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                # fields[5]=sectors_read, fields[9]=sectors_written (512 B each)
                return int(p[5]), int(p[9])
    return 0, 0

prev_cpu  = read_cpu()
prev_rd, prev_wr = read_disk(disk_device)
prev_t    = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s','cpu_user_pct','cpu_sys_pct',
                'cpu_iowait_pct','cpu_idle_pct',
                'disk_read_kbs','disk_write_kbs'])
    fh.flush()
    while True:
        time.sleep(1)
        now     = time.time()
        cur_cpu = read_cpu()
        cur_rd, cur_wr = read_disk(disk_device)

        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        user_pct    = 100.0 * delta[0] / total
        sys_pct     = 100.0 * delta[2] / total
        iowait_pct  = 100.0 * delta[4] / total
        idle_pct    = 100.0 * delta[3] / total

        dt = now - prev_t or 1
        rd_kbs = (cur_rd - prev_rd) * 512 / 1024 / dt
        wr_kbs = (cur_wr - prev_wr) * 512 / 1024 / dt

        w.writerow([int(now),
                    f'{user_pct:.2f}', f'{sys_pct:.2f}',
                    f'{iowait_pct:.2f}', f'{idle_pct:.2f}',
                    f'{rd_kbs:.1f}',    f'{wr_kbs:.1f}'])
        fh.flush()

        prev_cpu = cur_cpu
        prev_rd, prev_wr = cur_rd, cur_wr
        prev_t   = now
PYMON

    PLOTTER_SCRIPT=$(mktemp /tmp/slack_plotter_XXXXX.py)
    # (written later by write_plotter_script)
}

# ── Helpers ───────────────────────────────────────────────────────────────────

# Create a temp spec file = base workload + extra properties.
# Caller is responsible for deleting the returned file.
make_spec() {
    local base_spec="$1"; shift
    TMPSPEC=$(mktemp /tmp/slack_spec_XXXXX.spec)
    cp "$base_spec" "$TMPSPEC"
    # Append any additional key=value pairs passed as arguments
    for kv in "$@"; do
        echo "$kv" >> "$TMPSPEC"
    done
    echo "$TMPSPEC"
}

start_monitor() {
    local outfile="$1"
    python3 "$MONITOR_SCRIPT" "$outfile" "$DISK_DEVICE" &
    MONITOR_PID=$!
}

stop_monitor() {
    if [[ -n "${MONITOR_PID:-}" ]]; then
        kill "$MONITOR_PID" 2>/dev/null || true
        wait "$MONITOR_PID" 2>/dev/null || true
        MONITOR_PID=""
    fi
}

# Run ycsb_test with standardised flags.
# Usage: run_ycsb <spec> <dbpath> <threads> <bootstrap> <load> <run>
#                 <throughput> <xput_type> <runtime_s> <logfile>
run_ycsb() {
    local spec="$1" dbpath="$2" threads="$3"
    local bootstrap="$4" do_load="$5" do_run="$6"
    local do_xput="$7" xput_type="$8" runtime="$9" logfile="${10}"

    "$BINARY" \
        -db      baseline \
        -dbpath  "$dbpath" \
        -P       "$spec" \
        -bootstrap   "$bootstrap" \
        -threads     "$threads" \
        -load        "$do_load" \
        -run         "$do_run" \
        -throughput  "$do_xput" \
        -throughputtype "$xput_type" \
        -runtime     "$runtime" \
        -levels      6 \
        -table       baseline \
        2>&1 | tee "$logfile"
}

# Create the write-only workload spec used by E1/E3/E5.
create_write_spec() {
    local fieldlength="${1:-1024}"
    cat > "$WORKLOAD_DIR/slack_write.spec" << EOF
# Slack characterization: write-only workload (generated by slack_characterize.sh)
keylength=16
fieldcount=1
fieldlength=${fieldlength}
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
}

# ── Load phase (shared across experiments) ────────────────────────────────────

load_db() {
    local dbpath="$1" spec="$2" threads="${3:-8}"
    log "Loading $RECORD_COUNT records into $dbpath ..."
    # Use dirname in bash — avoids filesystem resolution of '../' when the
    # rocksdb/ subdirectory does not exist yet (RocksDB creates it on Open).
    run_ycsb "$spec" "$dbpath" "$threads" \
        true true false false 2 0 "$(dirname "$dbpath")/load.log"
    log "Load complete."
}

# ── E1: Write rate sweep ──────────────────────────────────────────────────────

run_e1() {
    sep
    log "E1: Write rate sweep (thread counts: $THREAD_COUNTS)"
    log "    Purpose: establish cpu_slack vs write_rate baseline curve"

    local exp_dir="$OUTPUT_DIR/E1_write_rate_sweep"
    mkdir -p "$exp_dir"

    create_write_spec 1024
    local base_spec="$WORKLOAD_DIR/slack_write.spec"
    local dbpath="$exp_dir/rocksdb"

    # Load once; reuse for all thread counts.
    local load_spec
    load_spec=$(make_spec "$base_spec")
    load_db "$dbpath" "$load_spec" 8
    rm -f "$load_spec"; TMPSPEC=""

    for threads in $THREAD_COUNTS; do
        sep
        log "E1: Running with $threads thread(s) for ${RUNTIME_SECS}s ..."

        local metrics_csv="$exp_dir/compaction_t${threads}.csv"
        local sys_csv="$exp_dir/system_t${threads}.csv"
        local log_file="$exp_dir/ycsb_t${threads}.log"

        local spec
        spec=$(make_spec "$base_spec" "metrics_output=$metrics_csv")

        start_monitor "$sys_csv"
        run_ycsb "$spec" "$dbpath" "$threads" \
            false false false true 2 "$RUNTIME_SECS" "$log_file"
        stop_monitor

        rm -f "$spec"; TMPSPEC=""
        log "  compaction metrics → $metrics_csv"
        log "  system CPU/IO      → $sys_csv"
    done

    log "E1 complete."
}

# ── E3: Breakdown point ───────────────────────────────────────────────────────
# Same as E1 but we add a few extra high-thread runs beyond the expected
# saturation point and watch when stall_micros_cumulative starts growing.

run_e3() {
    sep
    log "E3: Breakdown point characterization"
    log "    Purpose: find the write rate at which CPU slack disappears"
    log "    and write stalls begin.  Uses extended thread range."

    local exp_dir="$OUTPUT_DIR/E3_breakdown"
    mkdir -p "$exp_dir"

    # Extended thread list: go beyond E1 to reach saturation
    local all_threads="${THREAD_COUNTS} 48 64"

    create_write_spec 1024
    local base_spec="$WORKLOAD_DIR/slack_write.spec"
    local dbpath="$exp_dir/rocksdb"

    local load_spec
    load_spec=$(make_spec "$base_spec")
    load_db "$dbpath" "$load_spec" 8
    rm -f "$load_spec"; TMPSPEC=""

    for threads in $all_threads; do
        sep
        log "E3: Running $threads thread(s) for ${RUNTIME_SECS}s ..."

        local metrics_csv="$exp_dir/compaction_t${threads}.csv"
        local sys_csv="$exp_dir/system_t${threads}.csv"
        local log_file="$exp_dir/ycsb_t${threads}.log"

        local spec
        spec=$(make_spec "$base_spec" "metrics_output=$metrics_csv")

        start_monitor "$sys_csv"
        run_ycsb "$spec" "$dbpath" "$threads" \
            false false false true 2 "$RUNTIME_SECS" "$log_file"
        stop_monitor

        # Check if stalls appeared — print a quick summary
        local stall_max
        stall_max=$(awk -F, 'NR>1 && $20+0 > max {max=$20} END {print max+0}' \
                    "$metrics_csv" 2>/dev/null || echo 0)
        log "  max cumulative stall_micros = $stall_max µs"

        rm -f "$spec"; TMPSPEC=""
    done

    log "E3 complete."
}

# ── E5: Value size sweep ──────────────────────────────────────────────────────

run_e5() {
    sep
    log "E5: Value size sweep (fieldlengths: $FIELDLENGTHS)"
    log "    Purpose: measure how cpu_utilization changes with record size"
    log "    (direct input to the mRoutine cost model)"

    local exp_dir="$OUTPUT_DIR/E5_value_size"
    mkdir -p "$exp_dir"

    for fl in $FIELDLENGTHS; do
        sep
        log "E5: fieldlength=$fl bytes, threads=$E5_THREADS ..."

        local dbpath="$exp_dir/rocksdb_fl${fl}"
        local metrics_csv="$exp_dir/compaction_fl${fl}.csv"
        local sys_csv="$exp_dir/system_fl${fl}.csv"
        local log_file="$exp_dir/ycsb_fl${fl}.log"

        create_write_spec "$fl"
        local base_spec="$WORKLOAD_DIR/slack_write.spec"

        # Fresh DB per value size so compaction I/O pattern reflects record size.
        local load_spec
        load_spec=$(make_spec "$base_spec")
        load_db "$dbpath" "$load_spec" 8
        rm -f "$load_spec"; TMPSPEC=""

        local spec
        spec=$(make_spec "$base_spec" "metrics_output=$metrics_csv")

        start_monitor "$sys_csv"
        run_ycsb "$spec" "$dbpath" "$E5_THREADS" \
            false false false true 2 "$RUNTIME_SECS" "$log_file"
        stop_monitor

        rm -f "$spec"; TMPSPEC=""
        log "  compaction metrics → $metrics_csv"
    done

    log "E5 complete."
}

# ── E2 stub (hardware comparison) ────────────────────────────────────────────

run_e2_stub() {
    sep
    log "E2: Hardware comparison — STUB"
    log "    To run E2, execute this script twice with different settings:"
    log "      DISK_DEVICE=sda    DB_BASE_PATH=/mnt/sata/slack_exp  $0"
    log "      DISK_DEVICE=nvme0n1 DB_BASE_PATH=/mnt/nvme/slack_exp $0"
    log "    Then compare the E1 output directories from each run."
}

# ── E4 stub (compaction style) ────────────────────────────────────────────────

run_e4_stub() {
    sep
    log "E4: Compaction style comparison — STUB"
    log "    Requires adding '-compactionstyle universal|level' flag to"
    log "    ycsb_test and wiring it to options_.compaction_style."
    log "    Implement in db_helper.cc, then re-enable E4 here."
}

# ── Python plotter ────────────────────────────────────────────────────────────

write_plotter_script() {
    cat > "$PLOTTER_SCRIPT" << 'PYPLOT'
#!/usr/bin/env python3
"""
slack_plotter.py — generates PNG plots from slack_characterize.sh output.

Plots generated:
  plots/E1_cpu_utilization_per_thread.png  — per-compaction cpu_util over time
  plots/E1_cpu_slack_curve.png             — avg idle% vs thread count (KEY PLOT)
  plots/E1_write_amp.png                   — write amplification over time
  plots/E3_stall_micros.png                — cumulative stall µs vs thread count
  plots/E5_cpu_util_vs_value_size.png      — cpu_util vs record size
"""

import sys, os, glob, re
import pandas as pd
import matplotlib
matplotlib.use('Agg')          # no display needed — saves to PNG files
import matplotlib.pyplot as plt
import matplotlib.cm as cm
import numpy as np

OUTPUT_DIR = sys.argv[1]
PLOT_DIR   = os.path.join(OUTPUT_DIR, "plots")
os.makedirs(PLOT_DIR, exist_ok=True)

STYLE = {
    'figure.dpi':       150,
    'axes.spines.top':  False,
    'axes.spines.right':False,
    'axes.grid':        True,
    'grid.alpha':       0.4,
    'font.size':        11,
}
plt.rcParams.update(STYLE)

def load_compaction_csv(path):
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = (df['timestamp_us'] - df['timestamp_us'].iloc[0]) / 1e6
        return df
    except Exception as e:
        print(f"  [skip] {path}: {e}")
        return None

def load_system_csv(path):
    try:
        df = pd.read_csv(path)
        df['elapsed_s'] = df['timestamp_s'] - df['timestamp_s'].iloc[0]
        return df
    except Exception as e:
        print(f"  [skip] {path}: {e}")
        return None

def thread_count_from_path(path):
    m = re.search(r'_t(\d+)\.csv', os.path.basename(path))
    return int(m.group(1)) if m else 0

def fieldlength_from_path(path):
    m = re.search(r'_fl(\d+)\.csv', os.path.basename(path))
    return int(m.group(1)) if m else 0

# ── E1 plots ──────────────────────────────────────────────────────────────────

e1_dir = os.path.join(OUTPUT_DIR, "E1_write_rate_sweep")
if os.path.isdir(e1_dir):
    print("Plotting E1...")

    comp_files = sorted(glob.glob(os.path.join(e1_dir, "compaction_t*.csv")),
                        key=thread_count_from_path)
    sys_files  = sorted(glob.glob(os.path.join(e1_dir, "system_t*.csv")),
                        key=thread_count_from_path)

    colors = cm.viridis(np.linspace(0.15, 0.85, max(len(comp_files), 1)))

    # ── E1a: cpu_utilization over time per thread count ──
    fig, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=False)

    for i, fpath in enumerate(comp_files):
        df = load_compaction_csv(fpath)
        if df is None: continue
        t  = thread_count_from_path(fpath)
        axes[0].plot(df['elapsed_s'], df['cpu_utilization'],
                     color=colors[i], alpha=0.7, linewidth=0.8,
                     label=f'{t} thread{"s" if t>1 else ""}')
        axes[1].plot(df['elapsed_s'], df['write_amp'],
                     color=colors[i], alpha=0.7, linewidth=0.8,
                     label=f'{t}T')

    axes[0].set_ylabel('CPU utilization\n(cpu_micros / elapsed_micros)')
    axes[0].set_xlabel('Time into experiment (s)')
    axes[0].set_title('Compaction CPU utilization over time — E1 write rate sweep')
    axes[0].legend(loc='upper right', fontsize=9, ncol=2)

    axes[1].set_ylabel('Write amplification\n(output_bytes / input_bytes)')
    axes[1].set_xlabel('Time into experiment (s)')
    axes[1].set_title('Write amplification over time')
    axes[1].legend(loc='upper right', fontsize=9, ncol=2)

    plt.tight_layout()
    out = os.path.join(PLOT_DIR, "E1_cpu_utilization_per_thread.png")
    plt.savefig(out); plt.close()
    print(f"  → {out}")

    # ── E1b: KEY PLOT — avg cpu_idle% vs thread count (slack curve) ──
    summary_rows = []
    for sf in sys_files:
        df = load_system_csv(sf)
        if df is None: continue
        t  = thread_count_from_path(sf)
        # Skip first 10s warmup before averaging
        steady = df[df['elapsed_s'] > 10]
        if steady.empty: steady = df
        row = {
            'threads':      t,
            'avg_idle_pct': steady['cpu_idle_pct'].mean(),
            'avg_iowait_pct': steady['cpu_iowait_pct'].mean(),
            'disk_write_kbs': steady['disk_write_kbs'].mean(),
        }
        # Also pull avg cpu_utilization from compaction CSV
        cf = os.path.join(e1_dir, f"compaction_t{t}.csv")
        cdf = load_compaction_csv(cf)
        if cdf is not None:
            steady_c = cdf[cdf['elapsed_s'] > 10]
            row['avg_compaction_cpu_util'] = steady_c['cpu_utilization'].mean() if not steady_c.empty else float('nan')
            row['avg_pending_bytes_mb']    = steady_c['pending_compaction_bytes'].mean() / 1e6 if not steady_c.empty else float('nan')
            row['stall_appeared']          = int(steady_c['stall_micros_cumulative'].max() > 0)
        summary_rows.append(row)

    if summary_rows:
        sdf = pd.DataFrame(summary_rows).sort_values('threads')

        fig, axes = plt.subplots(1, 3, figsize=(14, 5))

        # Left: CPU idle% (≈ available slack)
        axes[0].plot(sdf['threads'], sdf['avg_idle_pct'],
                     'o-', color='steelblue', linewidth=2, markersize=7)
        axes[0].axhline(y=20, color='red', linestyle='--', alpha=0.6,
                        label='20% idle floor (tight)')
        axes[0].set_xlabel('Client write threads')
        axes[0].set_ylabel('Average CPU idle %')
        axes[0].set_title('CPU slack vs write rate\n(higher = more room for transforms)')
        axes[0].legend(fontsize=9)

        # Mark stall points
        stall_df = sdf[sdf.get('stall_appeared', pd.Series(0)) == 1]
        if not stall_df.empty:
            axes[0].scatter(stall_df['threads'], stall_df['avg_idle_pct'],
                           color='red', zorder=5, s=80, label='write stall')

        # Middle: compaction CPU utilization
        if 'avg_compaction_cpu_util' in sdf.columns:
            axes[1].plot(sdf['threads'], sdf['avg_compaction_cpu_util'],
                         's-', color='darkorange', linewidth=2, markersize=7)
            axes[1].set_xlabel('Client write threads')
            axes[1].set_ylabel('Avg compaction cpu_util\n(cpu_micros / elapsed_micros)')
            axes[1].set_title('Compaction CPU utilization\nvs write rate')

        # Right: pending compaction bytes (backpressure proxy)
        if 'avg_pending_bytes_mb' in sdf.columns:
            axes[2].plot(sdf['threads'], sdf['avg_pending_bytes_mb'],
                         '^-', color='forestgreen', linewidth=2, markersize=7)
            axes[2].set_xlabel('Client write threads')
            axes[2].set_ylabel('Avg pending compaction (MB)')
            axes[2].set_title('Compaction backlog\nvs write rate')

        plt.suptitle('E1 — Write rate sweep: CPU slack characterization',
                     fontsize=13, fontweight='bold')
        plt.tight_layout()
        out = os.path.join(PLOT_DIR, "E1_cpu_slack_curve.png")
        plt.savefig(out); plt.close()
        print(f"  → {out}  ← KEY PLOT")

        # Save summary CSV for the paper's analytic model
        sdf.to_csv(os.path.join(PLOT_DIR, "E1_summary.csv"), index=False)
        print(f"  → {os.path.join(PLOT_DIR, 'E1_summary.csv')}  (summary table)")

# ── E3 plot ───────────────────────────────────────────────────────────────────

e3_dir = os.path.join(OUTPUT_DIR, "E3_breakdown")
if os.path.isdir(e3_dir):
    print("Plotting E3...")

    comp_files = sorted(glob.glob(os.path.join(e3_dir, "compaction_t*.csv")),
                        key=thread_count_from_path)
    rows = []
    for fpath in comp_files:
        df = load_compaction_csv(fpath)
        if df is None: continue
        t          = thread_count_from_path(fpath)
        stall_max  = df['stall_micros_cumulative'].max()
        stall_delta = df['stall_micros_cumulative'].iloc[-1] - df['stall_micros_cumulative'].iloc[0]
        rows.append({
            'threads':            t,
            'stall_micros_final': stall_max,
            'stall_micros_delta': stall_delta,
            'avg_cpu_util':       df['cpu_utilization'].mean(),
            'avg_pending_mb':     df['pending_compaction_bytes'].mean() / 1e6,
        })

    if rows:
        sdf = pd.DataFrame(rows).sort_values('threads')

        fig, axes = plt.subplots(1, 2, figsize=(12, 5))

        axes[0].bar(sdf['threads'].astype(str), sdf['stall_micros_delta'] / 1e6,
                    color=['red' if v > 0 else 'steelblue'
                           for v in sdf['stall_micros_delta']])
        axes[0].set_xlabel('Client threads')
        axes[0].set_ylabel('Write stall duration in window (s)')
        axes[0].set_title('Write stall onset by thread count\n(red = stalls occurred)')

        # Mark the breakdown point
        first_stall = sdf[sdf['stall_micros_delta'] > 0]
        if not first_stall.empty:
            bp = first_stall.iloc[0]['threads']
            axes[0].annotate(f'Breakdown ≥ {bp} threads',
                            xy=(str(int(bp)), first_stall.iloc[0]['stall_micros_delta'] / 1e6),
                            xytext=(0.6, 0.9), textcoords='axes fraction',
                            arrowprops=dict(arrowstyle='->', color='darkred'),
                            color='darkred', fontsize=10)

        axes[1].plot(sdf['threads'], sdf['avg_cpu_util'],
                     'o-', color='darkorange', linewidth=2, markersize=7)
        axes[1].set_xlabel('Client threads')
        axes[1].set_ylabel('Avg compaction cpu_util')
        axes[1].set_title('Compaction CPU pressure\nat breakdown boundary')

        plt.suptitle('E3 — Breakdown point characterization',
                     fontsize=13, fontweight='bold')
        plt.tight_layout()
        out = os.path.join(PLOT_DIR, "E3_breakdown.png")
        plt.savefig(out); plt.close()
        print(f"  → {out}")

        sdf.to_csv(os.path.join(PLOT_DIR, "E3_summary.csv"), index=False)

# ── E5 plot ───────────────────────────────────────────────────────────────────

e5_dir = os.path.join(OUTPUT_DIR, "E5_value_size")
if os.path.isdir(e5_dir):
    print("Plotting E5...")

    comp_files = sorted(glob.glob(os.path.join(e5_dir, "compaction_fl*.csv")),
                        key=fieldlength_from_path)
    sys_files  = sorted(glob.glob(os.path.join(e5_dir, "system_fl*.csv")),
                        key=fieldlength_from_path)

    rows = []
    for fpath in comp_files:
        df  = load_compaction_csv(fpath)
        if df is None: continue
        fl  = fieldlength_from_path(fpath)
        steady = df[df['elapsed_s'] > 10]
        if steady.empty: steady = df
        row = {
            'fieldlength_bytes': fl,
            'record_size_bytes': fl + 16,      # +16 for key
            'avg_cpu_util':  steady['cpu_utilization'].mean(),
            'avg_write_amp': steady['write_amp'].mean(),
        }
        # Pull idle% from system CSV
        sf = os.path.join(e5_dir, f"system_fl{fl}.csv")
        sdf2 = load_system_csv(sf)
        if sdf2 is not None:
            ss = sdf2[sdf2['elapsed_s'] > 10]
            row['avg_idle_pct']    = ss['cpu_idle_pct'].mean() if not ss.empty else float('nan')
            row['avg_disk_wkbs']   = ss['disk_write_kbs'].mean() if not ss.empty else float('nan')
        rows.append(row)

    if rows:
        sdf = pd.DataFrame(rows).sort_values('record_size_bytes')

        fig, axes = plt.subplots(1, 3, figsize=(15, 5))

        axes[0].plot(sdf['record_size_bytes'], sdf['avg_cpu_util'],
                     'o-', color='steelblue', linewidth=2, markersize=7)
        axes[0].set_xscale('log')
        axes[0].set_xlabel('Record size (bytes, log scale)')
        axes[0].set_ylabel('Avg compaction cpu_util')
        axes[0].set_title('CPU utilization vs record size\n(basis for cost model)')

        axes[1].plot(sdf['record_size_bytes'], sdf.get('avg_idle_pct', [float('nan')]*len(sdf)),
                     's-', color='forestgreen', linewidth=2, markersize=7)
        axes[1].set_xscale('log')
        axes[1].set_xlabel('Record size (bytes, log scale)')
        axes[1].set_ylabel('Avg CPU idle %')
        axes[1].set_title('Available CPU slack vs record size')

        axes[2].plot(sdf['record_size_bytes'], sdf['avg_write_amp'],
                     '^-', color='darkorange', linewidth=2, markersize=7)
        axes[2].set_xscale('log')
        axes[2].set_xlabel('Record size (bytes, log scale)')
        axes[2].set_ylabel('Avg write amplification')
        axes[2].set_title('Write amplification vs record size')

        plt.suptitle('E5 — Value size sweep: cost model input',
                     fontsize=13, fontweight='bold')
        plt.tight_layout()
        out = os.path.join(PLOT_DIR, "E5_value_size.png")
        plt.savefig(out); plt.close()
        print(f"  → {out}")

        sdf.to_csv(os.path.join(PLOT_DIR, "E5_summary.csv"), index=False)

print("\nAll plots written to:", os.path.join(OUTPUT_DIR, "plots/"))
PYPLOT
}

run_plotter() {
    sep
    log "Generating plots..."
    write_plotter_script
    python3 "$PLOTTER_SCRIPT" "$OUTPUT_DIR"
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    log "slack_characterize.sh starting"
    log "  Binary:     $BINARY"
    log "  Output dir: $OUTPUT_DIR"
    log "  Device:     $DISK_DEVICE"
    log "  Records:    $RECORD_COUNT"
    log "  Runtime/pt: ${RUNTIME_SECS}s"
    echo ""

    check_prereqs
    setup_tmpfiles
    mkdir -p "$OUTPUT_DIR"

    [[ "$RUN_E1" == "1" ]] && run_e1
    [[ "$RUN_E3" == "1" ]] && run_e3
    [[ "$RUN_E5" == "1" ]] && run_e5

    run_e2_stub
    run_e4_stub

    run_plotter

    sep
    log "All experiments complete."
    log ""
    log "Results:  $OUTPUT_DIR"
    log "Plots:    $OUTPUT_DIR/plots/"
    log ""
    log "──────────────────────────────────────────────────────"
    log "View plots in your Mac browser WITHOUT scping files:"
    log ""
    log "  python3 -m http.server 8888 --directory $OUTPUT_DIR"
    log ""
    log "Then open: http://$(hostname -I | awk '{print $1}'):8888"
    log "──────────────────────────────────────────────────────"
}

main
