#!/usr/bin/env bash
# =============================================================================
# run_split_transform_profile.sh
#
# Runs the out-of-cache split_transform_bench C++ program while capturing 
# system resource metrics (CPU and Disk I/O) via a background monitor.
#
# Usage:
#   bash run_split_transform_profile.sh <duration_s> <n_workers> <num_fields> <field_length> <x_ways>
#
# Example:
#   bash experiment/slack/run_split_transform_profile.sh 10 4 16 128 4
# =============================================================================

set -euo pipefail

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <x_ways>"
    echo "Example: $0 10 4 16 128 4"
    exit 1
fi

DURATION_S="$1"
N_WORKERS="$2"
NUM_FIELDS="$3"
FIELD_LENGTH="$4"
X_WAYS="$5"

OUTPUT_DIR="./split_profile_$(date +%Y%m%d_%H%M%S)"
SYS_CSV="$OUTPUT_DIR/system.csv"
SUMMARY_TXT="$OUTPUT_DIR/summary.txt"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_SRC="$SCRIPT_DIR/split_transform_bench.cc"
BENCH_BIN=$(mktemp /tmp/split_bench_XXXXX)
MONITOR_SCRIPT=$(mktemp /tmp/split_monitor_XXXXX.py)
MONITOR_PID=""

log() { echo "[$(date '+%H:%M:%S')] $*"; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    rm -f "$BENCH_BIN" "$MONITOR_SCRIPT"
}
trap cleanup EXIT

# ── Compile Benchmark ─────────────────────────────────────────────────────────
log "Compiling split_transform_bench.cc ..."
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

log "Running split_transform_bench ..."
"$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" "$X_WAYS" | tee "$OUTPUT_DIR/benchmark.log"

log "Stopping monitor ..."
kill -TERM "$MONITOR_PID" 2>/dev/null || true
wait "$MONITOR_PID" 2>/dev/null || true
MONITOR_PID=""

# ── Summarize ─────────────────────────────────────────────────────────────────
sep
log "Generating resource summary ..."

python3 - "$SYS_CSV" "$SUMMARY_TXT" << 'PYSUMMARY'
import sys, csv, math

sys_csv       = sys.argv[1]
summary_txt   = sys.argv[2]

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

lines = [
    "=== Split Transformation Profile ===",
    f"Samples         : {len(rows)}",
    f"CPU Busy %      : {stats(cpu_busy)[0]:.2f} ± {stats(cpu_busy)[1]:.2f}",
    f"CPU IOWait %    : {stats(cpu_iowait)[0]:.2f} ± {stats(cpu_iowait)[1]:.2f}",
    f"Disk Read MB/s  : {stats(rd_mbs)[0]:.2f} ± {stats(rd_mbs)[1]:.2f}",
    f"Disk Write MB/s : {stats(wr_mbs)[0]:.2f} ± {stats(wr_mbs)[1]:.2f}",
]

out = "\n".join(lines)
print(out)
with open(summary_txt, 'w') as f:
    f.write(out + "\n")
PYSUMMARY

log "Profile complete! Results saved to $OUTPUT_DIR"
