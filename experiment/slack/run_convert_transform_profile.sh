#!/usr/bin/env bash
# =============================================================================
# run_convert_transform_profile.sh
#
# Profile convert_transform_bench: out-of-cache format conversion benchmark.
# Converts records between formats (csv2json, json2csv) or applies numeric type
# coercion (coerce) while capturing system CPU and disk I/O metrics.
#
# Usage:
#   bash run_convert_transform_profile.sh <duration_s> <n_workers> <num_fields> \
#       <field_length> <mode>
#
# Arguments:
#   duration_s    — benchmark duration in seconds
#   n_workers     — number of parallel worker threads
#   num_fields    — fields per record
#   field_length  — average bytes per field
#   mode          — conversion mode: csv2json | json2csv | coerce
#
# Examples:
#   bash experiment/slack/run_convert_transform_profile.sh 10 4 16 128 csv2json
#   bash experiment/slack/run_convert_transform_profile.sh 10 4 16 128 coerce
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib_transform_profile.sh
source "$SCRIPT_DIR/lib_transform_profile.sh"

# ── Argument parsing ──────────────────────────────────────────────────────────
if [ "$#" -lt 5 ]; then
    echo "Usage: $0 <duration_s> <n_workers> <num_fields> <field_length> <mode>"
    echo "  mode: csv2json | json2csv | coerce"
    echo "Example: $0 10 4 16 128 csv2json"
    exit 1
fi

DURATION_S="$1"
N_WORKERS="$2"
NUM_FIELDS="$3"
FIELD_LENGTH="$4"
MODE="$5"

case "$MODE" in
    csv2json|json2csv|coerce) ;;
    *) echo "ERROR: mode must be csv2json, json2csv, or coerce (got '$MODE')"; exit 1 ;;
esac

# ── Paths ─────────────────────────────────────────────────────────────────────
BENCH_SRC="$SCRIPT_DIR/convert_transform_bench.cc"
BENCH_BIN=$(mktemp /tmp/convert_transform_XXXXX)
MONITOR_SCRIPT=$(mktemp /tmp/transform_monitor_XXXXX.py)
MONITOR_PID=""
OUTPUT_DIR="./convert_profile_$(date +%Y%m%d_%H%M%S)_${MODE}"
SYS_CSV="$OUTPUT_DIR/system.csv"
SUMMARY_TXT="$OUTPUT_DIR/summary.txt"

setup_cleanup

# ── Compile ───────────────────────────────────────────────────────────────────
log "Compiling convert_transform_bench.cc ..."
g++ -O3 -pthread -o "$BENCH_BIN" "$BENCH_SRC" || { log "Compile failed"; exit 1; }

mkdir -p "$OUTPUT_DIR"

# ── Monitor ───────────────────────────────────────────────────────────────────
detect_disk_device
write_monitor_script

sep
start_monitor

# ── Run ───────────────────────────────────────────────────────────────────────
log "Running convert_transform_bench (mode=$MODE) ..."
"$BENCH_BIN" "$DURATION_S" "$N_WORKERS" "$NUM_FIELDS" "$FIELD_LENGTH" "$MODE" \
    | tee "$OUTPUT_DIR/benchmark.log"

stop_monitor

# ── Summary ───────────────────────────────────────────────────────────────────
sep
log "Generating resource summary ..."
run_summary convert

log "Profile complete! Results saved to $OUTPUT_DIR"
