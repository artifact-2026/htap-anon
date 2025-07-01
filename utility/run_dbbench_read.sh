#!/bin/bash

trap '' SIGPIPE  # Prevent dstat from crashing on broken pipes

set -euo pipefail

# === Configuration ===
LOG_DIR="../benchmark_logs"
TS=$(date +%s)

# Log file paths
DSTAT_LOG="$LOG_DIR/dstat_$TS.csv"
READ_LOG="$LOG_DIR/read_$TS.log"
VMSTAT_LOG="$LOG_DIR/vmstat_$TS.log"

# === Start Monitors ===
echo "Starting system monitors..."

# Adjust paths as necessary for your environment
unbuffer dstat --nocolor -cdngytlm 1 999999 > "$DSTAT_LOG" &
DSTAT_PID=$!

vmstat 1 > "$VMSTAT_LOG" &
VMSTAT_PID=$!

# === Handle cleanup ===
cleanup() {
    echo "Cleaning up monitor processes..."
    kill $DSTAT_PID $VMSTAT_PID  2>/dev/null || true
}
trap cleanup EXIT

./src/mycelium/db_bench \
  --db="/holly/htap/bench_runs/compaction_off" \
  --benchmarks=readrandom \
  --disable_auto_compactions=true \
  --max_background_compactions=0 \
  --max_background_flushes=0 \
  --level0_file_num_compaction_trigger=999999 \
  --level0_stop_writes_trigger=999999 \
  --level0_slowdown_writes_trigger=999999 \
  --num=5000000 \
  --reads=5000 \
  --threads=10 \
  --value_size=1000 \
  --key_size=16 \
  --compression_type=none \
  --use_existing_db=1 \
  --use_direct_reads \
  --use_direct_io_for_flush_and_compaction \
  --statistics \
  --histogram \
  > "$READ_LOG" 2>&1
