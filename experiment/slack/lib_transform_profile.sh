#!/usr/bin/env bash
# =============================================================================
# lib_transform_profile.sh — Shared infrastructure for transform bench harnesses
#
# Source this file from an individual harness after setting:
#   BENCH_BIN       — path returned by mktemp (set before calling setup_cleanup)
#   MONITOR_SCRIPT  — path returned by mktemp (set before calling setup_cleanup)
#   OUTPUT_DIR      — output directory path
#   SYS_CSV         — "$OUTPUT_DIR/system.csv"
#   SUMMARY_TXT     — "$OUTPUT_DIR/summary.txt"
#
# Provided functions (call in this order):
#   setup_cleanup          — install EXIT trap (call right after mktemp paths are set)
#   detect_disk_device     — sets DISK_DEVICE
#   write_monitor_script   — writes Python monitor to $MONITOR_SCRIPT
#   start_monitor          — launches monitor background process, sets MONITOR_PID
#   stop_monitor           — terminates monitor, waits for it to exit
#   run_summary <type>     — runs Python summary; type: split | convert | augment
# =============================================================================

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log() { echo "[$(date '+%H:%M:%S')] $*"; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

# ---------------------------------------------------------------------------
# Cleanup trap — sourcing script must call setup_cleanup after setting
# BENCH_BIN and MONITOR_SCRIPT.
# ---------------------------------------------------------------------------
cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    rm -f "${BENCH_BIN:-}" "${MONITOR_SCRIPT:-}"
}

setup_cleanup() {
    trap cleanup EXIT
}

# ---------------------------------------------------------------------------
# Disk device detection
# Sets: DISK_DEVICE
# ---------------------------------------------------------------------------
detect_disk_device() {
    DISK_DEVICE=$(df . 2>/dev/null | awk 'NR==2{print $1}' | awk -F/ '{print $NF}')
    DISK_DEVICE=$(echo "$DISK_DEVICE" | sed -E 's/p[0-9]+$//; s/[0-9]+$//')
}

# ---------------------------------------------------------------------------
# Write the Python system monitor to $MONITOR_SCRIPT.
# Reads /proc/stat and /proc/diskstats once per second; writes CSV rows to
# the file passed as its first argument.
# ---------------------------------------------------------------------------
write_monitor_script() {
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

# ---------------------------------------------------------------------------
# Monitor lifecycle
# ---------------------------------------------------------------------------
start_monitor() {
    log "Starting system monitor (logging to $SYS_CSV) ..."
    python3 "$MONITOR_SCRIPT" "$SYS_CSV" "${DISK_DEVICE:-}" &
    MONITOR_PID=$!
}

stop_monitor() {
    log "Stopping monitor ..."
    kill -TERM "${MONITOR_PID:-}" 2>/dev/null || true
    wait  "${MONITOR_PID:-}" 2>/dev/null || true
    MONITOR_PID=""
}

# ---------------------------------------------------------------------------
# Python summary
#   $1 = bench_type: split | convert | augment
#
# Requires globals: SYS_CSV, SUMMARY_TXT, OUTPUT_DIR
#
# Parses benchmark throughput from $OUTPUT_DIR/benchmark.log using the
# format appropriate for each bench type, then prints and saves a table of
# mean ± stddev system metrics alongside the throughput figure.
# ---------------------------------------------------------------------------
run_summary() {
    local bench_type="$1"
    python3 - "$SYS_CSV" "$SUMMARY_TXT" "$OUTPUT_DIR/benchmark.log" "$bench_type" <<'PYSUMMARY'
import sys, csv, math, re

sys_csv     = sys.argv[1]
summary_txt = sys.argv[2]
bench_log   = sys.argv[3]
bench_type  = sys.argv[4] if len(sys.argv) > 4 else "split"

# ── Parse throughput ────────────────────────────────────────────────────────
#   split:   rate=NNNN splits/s
#   convert: rate=NNNN rec/s   input_bw=NNNN MB/s
#   augment: rate=NNNN rec/s   input_bw=NNNN MB/s
#            + Hash collisions: avg_probe=NNNN steps/insert
#            + Sort batches:    entries_sorted=NNNN
xput     = "N/A"
bw_note  = ""
aux_note = ""
try:
    with open(bench_log) as f:
        content = f.read()
    if bench_type in ("convert", "augment"):
        m = re.search(r"rate=([0-9.]+)\s+rec/s", content)
        if m: xput = f"{float(m.group(1)):,.0f} rec/s"
        m2 = re.search(r"input_bw=([0-9.]+)\s+MB/s", content)
        if m2: bw_note = f"{float(m2.group(1)):.1f} MB/s input consumed"
        if bench_type == "augment":
            m3 = re.search(r"avg_probe=([0-9.]+)\s+steps/insert", content)
            if m3: aux_note = f"{float(m3.group(1)):.4f} avg probe steps/insert"
            m4 = re.search(r"entries_sorted=([0-9]+)", content)
            if m4: aux_note = f"{int(m4.group(1)):,} index entries sorted"
    else:
        m = re.search(r"rate=([0-9.]+)\s+splits/s", content)
        if m: xput = f"{float(m.group(1)):,.0f} splits/s"
except Exception:
    pass

# ── Load system CSV ─────────────────────────────────────────────────────────
rows = []
try:
    with open(sys_csv) as f:
        for r in csv.DictReader(f):
            rows.append(r)
except Exception:
    pass

if not rows:
    print("No system data recorded.")
    sys.exit(0)

rows = rows[1:] if len(rows) > 1 else rows  # skip first sample (init spike)

def stats(vals):
    n = len(vals)
    if n == 0: return 0.0, 0.0
    mean = sum(vals) / n
    var  = sum((v - mean) ** 2 for v in vals) / n
    return mean, math.sqrt(var)

cpu_busy   = [float(r['cpu_busy_pct'])   for r in rows]
cpu_iowait = [float(r['cpu_iowait_pct']) for r in rows]
rd_mbs     = [float(r['disk_read_mbs'])  for r in rows]
wr_mbs     = [float(r['disk_write_mbs']) for r in rows]

headers = {
    "split":   "=== Split Transformation Profile ===",
    "convert": "=== Conversion Transformation Profile ===",
    "augment": "=== Augment Transformation Profile ===",
}
header = headers.get(bench_type, "=== Transform Profile ===")

lines = [
    header,
    f"Samples         : {len(rows)}",
    f"CPU Busy %      : {stats(cpu_busy)[0]:.2f} \u00b1 {stats(cpu_busy)[1]:.2f}",
    f"CPU IOWait %    : {stats(cpu_iowait)[0]:.2f} \u00b1 {stats(cpu_iowait)[1]:.2f}",
    f"Disk Read MB/s  : {stats(rd_mbs)[0]:.2f} \u00b1 {stats(rd_mbs)[1]:.2f}",
    f"Disk Write MB/s : {stats(wr_mbs)[0]:.2f} \u00b1 {stats(wr_mbs)[1]:.2f}",
    "------------------------------------",
    f"Throughput      : {xput}",
]
if bw_note:
    lines.append(f"Input Bandwidth : {bw_note}")
if aux_note:
    lines.append(f"Index Aux       : {aux_note}")

out = "\n".join(lines)
print(out)
with open(summary_txt, 'w') as f:
    f.write(out + "\n")
PYSUMMARY
}
