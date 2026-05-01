#!/usr/bin/env bash
# =============================================================================
# run_augment_transform_profile.sh
#
# Profile augment_transform_bench: secondary index build benchmark.
# Scans base table records and builds an index over a user-specified composite
# key (one or more 0-based field positions) while capturing system metrics.
#
# Usage:
#   bash run_augment_transform_profile.sh <duration_s> <n_workers> <num_fields> \
#       <field_length> <index_mode> <field_idx0> [field_idx1 ...]
#
# Arguments:
#   duration_s    — benchmark duration in seconds
#   n_workers     — number of parallel worker threads
#   num_fields    — fields per record
#   field_length  — average bytes per field
#   index_mode    — index structure: hash | sort
#   field_idx     — 0-based field position(s) to include in the index key
#                   (at least one required; multiple = composite key)
#
# Examples:
#   bash experiment/slack/run_augment_transform_profile.sh 10 4 16 128 hash 2
#   bash experiment/slack/run_augment_transform_profile.sh 10 4 16 128 sort 0 3 7
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib_transform_profile.sh
source "$SCRIPT_DIR/lib_transform_profile.sh"

# ── Argument parsing ──────────────────────────────────────────────────────────
if [ "$#" -lt 6 ]; then
    echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <index_mode> <field_idx0> [field_idx1 ...]"
    echo "  index_mode: hash | sort"
    echo "  field_idx:  0-based field position(s) to index on"
    echo "Examples:"
    echo "  $0 10 4 16 128 hash 2"
    echo "  $0 10 4 16 128 sort 0 3 7"
    exit 1
fi

DURATION_S="$1"
N_WORKERS="$2"
NUM_FIELDS="$3"
FIELD_LENGTH="$4"
INDEX_MODE="$5"
shift 5
INDEX_FIELDS=("$@")   # all remaining args are field indices

case "$INDEX_MODE" in
    hash|sort) ;;
    *) echo "ERROR: index_mode must be 'hash' or 'sort' (got '$INDEX_MODE')"; exit 1 ;;
esac

# Describe the composite key for logging
INDEX_FIELDS_STR=$(IFS=,; echo "${INDEX_FIELDS[*]}")

# ── Paths ─────────────────────────────────────────────────────────────────────
BENCH_SRC="$SCRIPT_DIR/augment_transform_bench.cc"
BENCH_BIN=$(mktemp /tmp/augment_transform_XXXXX)
MONITOR_SCRIPT=$(mktemp /tmp/transform_monitor_XXXXX.py)
MONITOR_PID=""
OUTPUT_DIR="./augment_profile_$(date +%Y%m%d_%H%M%S)_${INDEX_MODE}"
SYS_CSV="$OUTPUT_DIR/system.csv"
SUMMARY_TXT="$OUTPUT_DIR/summary.txt"

setup_cleanup

# ── Compile ───────────────────────────────────────────────────────────────────
log "Compiling augment_transform_bench.cc ..."
g++ -O3 -std=c++11 -pthread -o "$BENCH_BIN" "$BENCH_SRC" || { log "Compile failed"; exit 1; }

mkdir -p "$OUTPUT_DIR"

# ── Monitor ───────────────────────────────────────────────────────────────────
detect_disk_device
write_monitor_script

sep
start_monitor

# ── Run ───────────────────────────────────────────────────────────────────────
log "Running augment_transform_bench (mode=$INDEX_MODE, key=[$INDEX_FIELDS_STR]) ..."
"$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" \
    "$INDEX_MODE" "${INDEX_FIELDS[@]}" \
    | tee "$OUTPUT_DIR/benchmark.log"

stop_monitor

# ── Summary ───────────────────────────────────────────────────────────────────
sep
log "Generating resource summary ..."
run_summary augment

log "Profile complete! Results saved to $OUTPUT_DIR"
