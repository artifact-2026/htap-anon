#!/usr/bin/env bash
# =============================================================================
# run_split_transform_profile.sh
#
# Profile split_transform_bench: out-of-cache CSV/JSON column-split benchmark.
# Splits each record (num_fields × field_length bytes) into x_ways partitions
# while capturing system CPU and disk I/O metrics via a background monitor.
#
# Usage:
#   bash run_split_transform_profile.sh <duration_s> <n_workers> <num_fields> \
#       <field_length> <x_ways> [csv|json]
#
# Arguments:
#   duration_s    — benchmark duration in seconds
#   n_workers     — number of parallel worker threads
#   num_fields    — fields per record
#   field_length  — average bytes per field
#   x_ways        — number of output partitions (must be <= num_fields)
#   format        — input format: csv (default) or json
#
# Examples:
#   bash experiment/slack/run_split_transform_profile.sh 10 4 16 128 4
#   bash experiment/slack/run_split_transform_profile.sh 10 4 16 128 2 json
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib_transform_profile.sh
source "$SCRIPT_DIR/lib_transform_profile.sh"

# ── Argument parsing ──────────────────────────────────────────────────────────
if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]"
    echo "Example: $0 10 4 16 128 4 json"
    exit 1
fi

DURATION_S="$1"
N_WORKERS="$2"
NUM_FIELDS="$3"
FIELD_LENGTH="$4"
X_WAYS="$5"
FORMAT="${6:-csv}"
FORMAT=$(echo "$FORMAT" | tr '[:upper:]' '[:lower:]')

# Validate all numeric arguments upfront — catches e.g. 'json' passed as x_ways
for _var_name in DURATION_S N_WORKERS NUM_FIELDS FIELD_LENGTH X_WAYS; do
    _val="${!_var_name}"
    _label=$(echo "$_var_name" | tr '[:upper:]' '[:lower:]')
    if ! [[ "$_val" =~ ^[1-9][0-9]*$ ]]; then
        echo "ERROR: <${_label}> must be a positive integer (got '$_val')"
        echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]"
        exit 1
    fi
done

if [ "$FORMAT" != "csv" ] && [ "$FORMAT" != "json" ]; then
    echo "ERROR: format must be 'csv' or 'json' (got '$FORMAT')"
    exit 1
fi

# ── Paths ─────────────────────────────────────────────────────────────────────
BENCH_SRC="$SCRIPT_DIR/split_transform_bench.cc"
BENCH_BIN=$(mktemp /tmp/split_transform_XXXXX)
MONITOR_SCRIPT=$(mktemp /tmp/transform_monitor_XXXXX.py)
MONITOR_PID=""
OUTPUT_DIR="./split_profile_$(date +%Y%m%d_%H%M%S)"
SYS_CSV="$OUTPUT_DIR/system.csv"
SUMMARY_TXT="$OUTPUT_DIR/summary.txt"

setup_cleanup

# ── Compile ───────────────────────────────────────────────────────────────────
log "Compiling split_transform_bench.cc ..."
g++ -O3 -pthread -o "$BENCH_BIN" "$BENCH_SRC" || { log "Compile failed"; exit 1; }

mkdir -p "$OUTPUT_DIR"

# ── Monitor ───────────────────────────────────────────────────────────────────
detect_disk_device
write_monitor_script

sep
start_monitor

# ── Run ───────────────────────────────────────────────────────────────────────
log "Running split_transform_bench (format=$FORMAT, x_ways=$X_WAYS) ..."
"$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" "$X_WAYS" "$FORMAT" \
    | tee "$OUTPUT_DIR/benchmark.log"

stop_monitor

# ── Summary ───────────────────────────────────────────────────────────────────
sep
log "Generating resource summary ..."
run_summary split

log "Profile complete! Results saved to $OUTPUT_DIR"
