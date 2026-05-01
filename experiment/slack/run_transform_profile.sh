#!/usr/bin/env bash
# =============================================================================
# run_transform_profile.sh
#
# Runs either split_transform_bench or convert_transform_bench while capturing
# system resource metrics (CPU and Disk I/O) via a background monitor.
#
# Usage:
#   bash run_transform_profile.sh <duration_s> <n_workers> <num_fields> \
#       <field_length> <x_ways_or_mode> [csv|json] [--bench split|convert]
#
# Benchmark selection (--bench):
#   split   — split_transform_bench: splits records into X_WAYS partitions.
#             Extra arg: x_ways (integer), optional format [csv|json].
#   convert — convert_transform_bench: converts record format in-place.
#             Extra arg: mode (csv2json|json2csv|coerce), format arg ignored.
#
# Examples:
#   bash experiment/slack/run_transform_profile.sh 10 4 16 128 4 json
#   bash experiment/slack/run_transform_profile.sh 10 4 16 128 csv2json --bench convert
# =============================================================================

set -euo pipefail

if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <x_ways_or_mode> [csv|json] [--bench split|convert]"
    echo "  --bench split   (default) run split_transform_bench"
    echo "  --bench convert           run convert_transform_bench"
    exit 1
fi

DURATION_S="$1"
N_WORKERS="$2"
NUM_FIELDS="$3"
FIELD_LENGTH="$4"
X_WAYS_OR_MODE="$5"

# Parse remaining optional args: [format] [--bench split|convert]
FORMAT="csv"
BENCH_TYPE="split"
shift 5
while [ "$#" -gt 0 ]; do
    case "$1" in
        --bench)
            shift
            BENCH_TYPE="${1:-split}"
            ;;
        csv|json|CSV|JSON)
            FORMAT=$(echo "$1" | tr '[:upper:]' '[:lower:]')
            ;;
        *)  # positional: treat as format if not already set via --bench
            FORMAT=$(echo "$1" | tr '[:upper:]' '[:lower:]')
            ;;
    esac
    shift
done
BENCH_TYPE=$(echo "$BENCH_TYPE" | tr '[:upper:]' '[:lower:]')

if [ "$BENCH_TYPE" != "split" ] && [ "$BENCH_TYPE" != "convert" ]; then
    echo "ERROR: --bench must be 'split' or 'convert' (got '$BENCH_TYPE')"
    exit 1
fi

OUTPUT_DIR="./transform_profile_$(date +%Y%m%d_%H%M%S)_${BENCH_TYPE}"
SYS_CSV="$OUTPUT_DIR/system.csv"
SUMMARY_TXT="$OUTPUT_DIR/summary.txt"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ "$BENCH_TYPE" = "convert" ]; then
    BENCH_SRC="$SCRIPT_DIR/convert_transform_bench.cc"
else
    BENCH_SRC="$SCRIPT_DIR/split_transform_bench.cc"
fi
BENCH_BIN=$(mktemp /tmp/transform_bench_XXXXX)
MONITOR_SCRIPT=$(mktemp /tmp/transform_monitor_XXXXX.py)
MONITOR_PID=""

log() { echo "[$(date '+%H:%M:%S')] $*"; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    rm -f "$BENCH_BIN" "$MONITOR_SCRIPT"
}
trap cleanup EXIT

# ── Compile Benchmark ─────────────────────────────────────────────────────────
log "Compiling $(basename "$BENCH_SRC") ..."
g++ -O3 -pthread -o "$BENCH_BIN" "$BENCH_SRC" || { log "Compile failed"; exit 1; }

mkdir -p "$OUTPUT_DIR"

# ── Setup System Monitor ──────────────────────────────────────────────────────
# Detect disk device to monitor
disk_device=$(df . 2>/dev/null | awk 'NR==2{print $1}' | awk -F/ '{print $NF}')
disk_device=$(echo "$disk_device" | sed -E 's/p[0-9]+$//; s/[0-9]+$//')

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

# ── Run Profile ───────────────────────────────────────────────────────────────
sep
log "Starting system monitor (logging to $SYS_CSV) ..."
python3 "$MONITOR_SCRIPT" "$SYS_CSV" "$disk_device" &
MONITOR_PID=$!

if [ "$BENCH_TYPE" = "convert" ]; then
    # convert_transform_bench: <duration_s> <n_workers> <num_fields> <field_length> <mode>
    # X_WAYS_OR_MODE holds the conversion mode (csv2json|json2csv|coerce)
    log "Running convert_transform_bench (mode=$X_WAYS_OR_MODE) ..."
    "$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" "$X_WAYS_OR_MODE" \
        | tee "$OUTPUT_DIR/benchmark.log"
else
    # split_transform_bench: <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]
    log "Running split_transform_bench ($FORMAT format, x_ways=$X_WAYS_OR_MODE) ..."
    "$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" "$X_WAYS_OR_MODE" "$FORMAT" \
        | tee "$OUTPUT_DIR/benchmark.log"
fi

log "Stopping monitor ..."
kill -TERM "$MONITOR_PID" 2>/dev/null || true
wait "$MONITOR_PID" 2>/dev/null || true
MONITOR_PID=""

# ── Summarize ─────────────────────────────────────────────────────────────────
sep
log "Generating resource summary ..."

BENCH_TYPE_PY="$BENCH_TYPE"
python3 - "$SYS_CSV" "$SUMMARY_TXT" "$OUTPUT_DIR/benchmark.log" "$BENCH_TYPE_PY" <<'PYSUMMARY'
import sys, csv, math, re

sys_csv     = sys.argv[1]
summary_txt = sys.argv[2]
bench_log   = sys.argv[3]
bench_type  = sys.argv[4] if len(sys.argv) > 4 else "split"

# Parse throughput from benchmark log — handle both output formats:
#   split:   rate=NNNN splits/s
#   convert: rate=NNNN rec/s  input_bw=NNNN MB/s
xput    = "N/A"
bw_note = ""
try:
    with open(bench_log) as f:
        content = f.read()
    if bench_type == "convert":
        m = re.search(r"rate=([0-9.]+)\s+rec/s", content)
        if m: xput = f"{float(m.group(1)):,.0f} rec/s"
        m2 = re.search(r"input_bw=([0-9.]+)\s+MB/s", content)
        if m2: bw_note = f"{float(m2.group(1)):.1f} MB/s input consumed"
    else:
        m = re.search(r"rate=([0-9.]+)\s+splits/s", content)
        if m: xput = f"{float(m.group(1)):,.0f} splits/s"
except:
    pass

rows = []
try:
    with open(sys_csv) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)
except:
    pass

if not rows:
    print("No system data recorded.")
    sys.exit(0)

# Skip the first 1s sample to avoid initialization spike
rows = rows[1:] if len(rows) > 1 else rows

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

header = "=== Conversion Transformation Profile ===" if bench_type == "convert" \
         else "=== Split Transformation Profile ==="

lines = [
    header,
    f"Samples         : {len(rows)}",
    f"CPU Busy %      : {stats(cpu_busy)[0]:.2f} ± {stats(cpu_busy)[1]:.2f}",
    f"CPU IOWait %    : {stats(cpu_iowait)[0]:.2f} ± {stats(cpu_iowait)[1]:.2f}",
    f"Disk Read MB/s  : {stats(rd_mbs)[0]:.2f} ± {stats(rd_mbs)[1]:.2f}",
    f"Disk Write MB/s : {stats(wr_mbs)[0]:.2f} ± {stats(wr_mbs)[1]:.2f}",
    "------------------------------------",
    f"Throughput      : {xput}",
]
if bw_note:
    lines.append(f"Input Bandwidth : {bw_note}")

out = "\n".join(lines)
print(out)
with open(summary_txt, 'w') as f:
    f.write(out + "\n")
PYSUMMARY

log "Profile complete! Results saved to $OUTPUT_DIR"
