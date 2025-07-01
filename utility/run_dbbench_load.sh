#!/bin/bash

trap '' SIGPIPE  # Prevent dstat from crashing on broken pipes

set -euo pipefail

# === Configuration ===
LOG_DIR="../benchmark_logs"
mkdir -p "$LOG_DIR"
TS=$(date +%s)

# Log file paths
DSTAT_LOG="$LOG_DIR/dstat_$TS.csv"
IOSTAT_LOG="$LOG_DIR/iostat_$TS.log"
VMSTAT_LOG="$LOG_DIR/vmstat_$TS.log"
IOTOP_LOG="$LOG_DIR/iotop_$TS.log"
DB_BENCH_LOG="$LOG_DIR/db_bench_$TS.log"

# === Start Monitors ===
echo "Starting system monitors..."

# Adjust paths as necessary for your environment
unbuffer dstat --nocolor -cdngytlm 1 999999 > "$DSTAT_LOG" &
DSTAT_PID=$!

iostat -xm 1 > "$IOSTAT_LOG" &
IOSTAT_PID=$!

vmstat 1 > "$VMSTAT_LOG" &
VMSTAT_PID=$!

sudo iotop -botqqq --iter=999999 > "$IOTOP_LOG" &
IOTOP_PID=$!

# === Handle cleanup ===
cleanup() {
    echo "Cleaning up monitor processes..."
    kill $DSTAT_PID $IOSTAT_PID $VMSTAT_PID $IOTOP_PID 2>/dev/null || true
}
trap cleanup EXIT

# === Run db_bench ===
echo "Running db_bench..."
#DB_PATH="/holly/htap/bench_runs/io_profile"
#DB_PATH="/holly/htap/bench_runs/io_stress_rate_limit_250M"
DB_PATH="/holly/htap/bench_runs/compaction_off"
mkdir -p "$DB_PATH"

./src/mycelium/db_bench \
  --db="$DB_PATH" \
  --benchmarks=fillrandom \
  --num=5000000 \
  --value_size=1000 \
  --key_size=16 \
  --threads=10 \
  --compression_type=none \
  --disable_auto_compactions=true \
  --use_existing_db=0 \
  --level0_file_num_compaction_trigger=999999 \
  --level0_slowdown_writes_trigger=999999 \
  --level0_stop_writes_trigger=999999 \
  --use_direct_reads \
  --use_direct_io_for_flush_and_compaction \
  > "$DB_BENCH_LOG" 2>&1

#./src/mycelium/db_bench \
#  --db="$DB_PATH" \
#  --benchmarks=compact \
#  --use_existing_db=true \
#  --compression_type=none \
#  --disable_auto_compactions=true \
#  > "$DB_BENCH_LOG" 2>&1
#  --use_direct_reads \
#  --use_direct_io_for_flush_and_compaction \
#  --rate_limiter_bytes_per_sec=262144000 \

#./src/mycelium/db_bench \
#  --db="$DB_PATH" \
#  --benchmarks=fillrandom \
#  --num=5000000 \
#  --value_size=1000 \
#  --key_size=16 \
#  --threads=10 \
#  --compression_type=none \
#  --use_existing_db=0 \
#  --level0_file_num_compaction_trigger=2 \
#  --level0_slowdown_writes_trigger=8 \
#  --level0_stop_writes_trigger=12 \
#  --max_background_jobs=8 \
#  --subcompactions=4 \
#  --use_direct_reads \
#  --use_direct_io_for_flush_and_compaction \
#  > "$DB_BENCH_LOG" 2>&1

# === Done ===
echo "Benchmark complete."
echo "Logs saved to: $LOG_DIR"
ls -lh "$LOG_DIR"/*"$TS"*
