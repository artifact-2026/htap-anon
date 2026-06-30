#!/bin/bash
set -euo pipefail

DB_PATH="/data/bench_results"
DEVICE=$(df "$DB_PATH" | tail -1 | awk '{print $1}')
THREADS=8
NUM_BASE=200000            # per run
WB_SIZE=67108864
MAX_COMPACTIONS=32
CPU_CORES="0-15"             # pin to these CPU cores
VALUE_SIZES=(512 1024 2048 4096 8192 16384 32768 65536 131072)

echo "Using device: $DEVICE"
echo "CPU cores pinned: $CPU_CORES"
echo

# Before the loop
LAST_IOSTAT_PID=""
LAST_VMSTAT_PID=""

for VSIZE in "${VALUE_SIZES[@]}"; do
    echo "=============================="
    echo " Running with value_size=$VSIZE"
    echo "=============================="

    # 0️⃣ Safety: kill leftover monitors from previous runs
    if [[ -n "$LAST_IOSTAT_PID" ]] && kill -0 "$LAST_IOSTAT_PID" 2>/dev/null; then
        kill "$LAST_IOSTAT_PID"
    fi
    if [[ -n "$LAST_VMSTAT_PID" ]] && kill -0 "$LAST_VMSTAT_PID" 2>/dev/null; then
        kill "$LAST_VMSTAT_PID"
    fi

    # 1️⃣ Clear DB and caches
    rm -rf "$DB_PATH"
    mkdir -p "$DB_PATH"
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

    # 2️⃣ Log CPU frequency & temperature before ru
    echo "--- CPU state before run ---"
    lscpu | grep "MHz"
    if command -v sensors >/dev/null 2>&1; then
        sensors
    else
        echo "(sensors command not found)"
    fi
    echo "---------------------------"

    # 3️⃣ Start monitoring
    : > vmstat_log.txt
    : > iostat_log.txt
    iostat -dx $DEVICE 1 > iostat_log.txt &
    IOSTAT_PID=$!
    vmstat 1 > vmstat_log.txt &
    VMSTAT_PID=$!

    # 4️⃣ Run db_bench pinned to cores
    taskset -c $CPU_CORES ./src/mycelium/db_bench \
      --benchmarks=fillrandom \
      --num=$NUM_BASE \
      --key_size=16 \
      --value_size=$VSIZE \
      --compression_type=none \
      --threads=$THREADS \
      --disable_wal=false \
      --write_buffer_size=$WB_SIZE \
      --db=$DB_PATH \
      --max_background_compactions=$MAX_COMPACTIONS \
      --subcompactions=16 \
      --max_background_jobs=64 \
      --stats_interval_seconds=5

    # 5️⃣ Stop monitoring
    set +e
    kill "$IOSTAT_PID" 2>/dev/null || true
    kill "$VMSTAT_PID" 2>/dev/null || true
    wait "$IOSTAT_PID" 2>/dev/null || true
    wait "$VMSTAT_PID" 2>/dev/null || true
    set -e

    # Store for next iteration's safety kill
    LAST_IOSTAT_PID=$IOSTAT_PID
    LAST_VMSTAT_PID=$VMSTAT_PID

    # 6️⃣ Analyze results
    echo "--- Analysis for value_size=$VSIZE ---"
    ./analyze_baseline.sh
    echo
done
