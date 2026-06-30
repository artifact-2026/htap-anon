#!/bin/bash

set -euo pipefail

# CONFIG
DB_BENCH_CMD="./src/mycelium/db_bench --benchmarks=fillrandom --num=2500000 --key_size=16 --value_size=1000 --compression_type=none --threads=4 --disable_wal=true --write_buffer_size=67108864 --db=/data/bench_results --max_background_compactions=8 --stats_interval_seconds=5"

echo "Starting RocksDB benchmark..."
$DB_BENCH_CMD &

BENCH_PID=$!

# --- Wait until 60s ---
sleep 60

# === Interval: 60–120s === IO contention (25% of IO bandwidth)
echo "Starting IO contention from 60s to 120s..."
fio --name=io_competitor --runtime=60 --time_based --rw=write --bs=4k --size=1G \
    --filename=tempfile1 --iodepth=16 --ioengine=libaio --rate=194m &

sleep 60  # now at 120

# --- Wait until 240s ---
sleep 120  # from 120 to 240

# === Interval: 240–300s === CPU contention (25% of CPU)
echo "Starting CPU contention from 240s to 300s..."
stress-ng --cpu 2 --timeout 60 &  # adjust if your machine has different core count

# Wait for db_bench to finish
wait $BENCH_PID

echo "Cleaning up..."
rm -f tempfile1
