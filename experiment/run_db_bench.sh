#!/bin/bash

set -euo pipefail

# CONFIG
DB_BENCH_CMD="./src/mycelium/db_bench --benchmarks=fillrandom --num=400000 --key_size=16 --value_size=1024 --compression_type=none --threads=8 --disable_wal=false --write_buffer_size=33554432 --db=/data/bench_results --max_background_compactions=8 --stats_interval_seconds=5"

echo "Starting system monitoring..."

# Start monitoring disk I/O
iostat -dx /dev/nvme0n1p4 1 > iostat_log.txt &
IOSTAT_PID=$!

# Start monitoring CPU usage and memory
vmstat 1 > vmstat_log.txt &
VMSTAT_PID=$!

echo "Starting RocksDB benchmark..."
$DB_BENCH_CMD

# Kill monitoring tools after benchmark completes
echo "Stopping system monitoring..."
kill $IOSTAT_PID
kill $VMSTAT_PID

echo "Done. Logs written to vmstat_log.csv and iostat_log.txt"
