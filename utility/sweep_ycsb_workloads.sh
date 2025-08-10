#!/bin/bash
set -euo pipefail

DB_PATH="/holly/ycsb_results"
DEVICE=$(df "$DB_PATH" | tail -1 | awk '{print $1}')
THREADS=8
NUM_BASE=200000            # per run
WB_SIZE=67108864
MAX_COMPACTIONS=32
CPU_CORES="0-15"             # pin to these CPU cores
VALUE_SIZES=(512 1024 2048 4096 8192 16384 32768)

echo "Using device: $DEVICE"
echo "CPU cores pinned: $CPU_CORES"
echo

# Before the loop
LAST_IOSTAT_PID=""
LAST_VMSTAT_PID=""

WORKLOADS=("test_workloadi.spec")

for WORKLOAD in "${WORKLOADS[@]}"; do
    echo "================================"
    echo " Running with workload=$WORKLOAD"
    echo "================================"

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

    # 2️⃣ Log CPU frequency & temperature before run
    echo "--- CPU state before run ---"
    lscpu | grep "MHz"
    if command -v sensors >/dev/null 2>&1; then
        sensors
    else
        echo "(sensors command not found)"
    fi
    echo "---------------------------"

    # 3️⃣  Load Dat
    rm -rf /holly/ycsb_results
    ./src/test/ycsb/ycsb_test \
            -db baseline \
            -dbpath /holly/ycsb_results \
            -P "../src/test/ycsb/workloads/$WORKLOAD" \
            -bootstrap true -threads 8 -load true -run false -throughput false

    # 4 Start monitoring
    : > vmstat_log.txt
    : > iostat_log.txt
    iostat -dx $DEVICE 1 > iostat_log.txt &
    IOSTAT_PID=$!
    vmstat 1 > vmstat_log.txt &
    VMSTAT_PID=$!

    # 5 Run db_bench pinned to cores
    taskset -c $CPU_CORES ./src/test/ycsb/ycsb_test \
            -db baseline \
            -dbpath /holly/ycsb_results \
            -P "../src/test/ycsb/workloads/$WORKLOAD" \
            -bootstrap false -threads 8 -load false -run true -throughput false

    # 6 Stop monitoring
    set +e
    kill "$IOSTAT_PID" 2>/dev/null || true
    kill "$VMSTAT_PID" 2>/dev/null || true
    wait "$IOSTAT_PID" 2>/dev/null || true
    wait "$VMSTAT_PID" 2>/dev/null || true
    set -e

    # Store for next iteration's safety kill
    LAST_IOSTAT_PID=$IOSTAT_PID
    LAST_VMSTAT_PID=$VMSTAT_PID

    # 7 Analyze results
    echo "--- Analysis for workload=$WORKLOAD ---"
    ./analyze_baseline.sh
    echo
done
