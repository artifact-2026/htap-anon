#!/usr/bin/env bash

# run_mycelium.sh – Mycelium experiment runner with optional baseline comparison
#
# Usage:
#   ./run_mycelium.sh <BUILD_DIR> [RUNTIME_SECS] [THREADS] [OUTPUT_DIR] [--comparison] [--transform <type>]
#
# Arguments:
#   BUILD_DIR    Path to the CMake build directory containing ycsb_test
#   RUNTIME_SECS Throughput measurement window in seconds (default: 300)
#   THREADS      Number of client threads (default: 16)
#   OUTPUT_DIR   Directory for CSV output (default: ./results)
#
# Flags:
#   --comparison          Run the baseline (plain RocksDB) phase before the Mycelium phase.
#                         Default: false (baseline is skipped).
#   --transform <type>    Transform to run. Supported values: splitting, converting, indexing.
#                         Default: splitting.
#
# Output CSV files (appended on each run):
#   baseline_results.csv   – baseline (plain RocksDB) results (if --comparison is true)
#   split_results.csv      – results for the "splitting" transform
#   convert_results.csv    – results for the "converting" transform
#   index_results.csv      – results for the "indexing" transform
#
# Cleanup is performed after every execution (both baseline and transform phases).

set -euo pipefail

# -------------------------------------------------
# Default flag values
# -------------------------------------------------
COMPARISON=false          # --comparison flag (default: false)
TRANSFORM="splitting"    # --transform flag (default)

# -------------------------------------------------
# Parse flags and positional arguments
# -------------------------------------------------
POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --comparison)
      if [[ $# -gt 1 && ( "$2" == "true" || "$2" == "false" ) ]]; then
        COMPARISON="$2"
        shift 2
      else
        COMPARISON=true
        shift
      fi
      ;;
    --transform)
      TRANSFORM="${2:-splitting}"
      shift 2
      ;;
    *)
      POSITIONAL_ARGS+=("$1")
      shift
      ;;
  esac
done

# Restore positional parameters
set -- "${POSITIONAL_ARGS[@]}"

# -------------------------------------------------
# Positional arguments (still required)
# -------------------------------------------------
BUILD_DIR=${1:?Usage: $0 <BUILD_DIR> [RUNTIME_SECS] [THREADS] [OUTPUT_DIR]}
RUNTIME=${2:-300}
THREADS=${3:-16}
OUTPUT_DIR=${4:-results}
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORKLOAD=$SCRIPT_DIR/workloads/test_workloada.spec
YCSB=$BUILD_DIR/ycsb_test

BASELINE_DB="/holly/results/bench-baseline"
SPLITTING_DB="/holly/results/bench-splitting"

# -------------------------------------------------
# CSV file paths (named per user request)
# -------------------------------------------------
BASELINE_CSV="${OUTPUT_DIR}/baseline_results.csv"
SPLIT_CSV="${OUTPUT_DIR}/split_results.csv"
CONVERT_CSV="${OUTPUT_DIR}/convert_results.csv"
INDEX_CSV="${OUTPUT_DIR}/index_results.csv"

# Warm‑up period to skip (seconds). First 60 s are often dominated by memtable flushes.
SKIP=60
# Throughput aggregation window (seconds).
XPUT_WINDOW=10

# -------------------------------------------------
# Sanity checks
# -------------------------------------------------
if [[ ! -x "${YCSB}" ]]; then
  echo "ERROR: ycsb_test binary not found at '${YCSB}'"
  echo "Build the project first:  cmake --build <BUILD_DIR> --target ycsb_test"
  exit 1
fi

if [[ ! -f "${WORKLOAD}" ]]; then
  echo "ERROR: workload spec not found at '${WORKLOAD}'"
  exit 1
fi

if (( RUNTIME <= SKIP )); then
  echo "ERROR: RUNTIME (${RUNTIME}s) must be greater than SKIP (${SKIP}s)"
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"

# -------------------------------------------------
# Helper: run one phase and echo a label
# -------------------------------------------------
run_phase() {
  local label="$1"; shift
  echo "[${label}] $*"
  "$@"
  echo "[${label}] done"
  echo
}

# -------------------------------------------------
# Baseline phase (plain RocksDB)
# -------------------------------------------------
if $COMPARISON; then
  echo "──────────────────────────────────────"
  echo "  BASELINE (plain RocksDB)"
  echo "──────────────────────────────────────"

  echo "Removing old baseline DB at ${BASELINE_DB} ..."
  rm -rf "${BASELINE_DB}"

  run_phase "baseline/load" "${YCSB}" \
    -P "${WORKLOAD}" \
    -db baseline \
    -dbpath "${BASELINE_DB}" \
    -bootstrap true \
    -load true \
    -threads "${THREADS}"

  run_phase "baseline/run" "${YCSB}" \
    -P "${WORKLOAD}" \
    -db baseline \
    -dbpath "${BASELINE_DB}" \
    -bootstrap false \
    -throughput true \
    -runtime "${RUNTIME}" \
    -skip "${SKIP}" \
    -xputwindow "${XPUT_WINDOW}" \
    -xputfile "${BASELINE_CSV}" \
    -threads "${THREADS}"

  echo "Baseline CSV appended to: ${BASELINE_CSV}"
  echo
else
  echo "Skipping baseline phase (--comparison=false)."
fi

# -------------------------------------------------
# Transform phase (Mycelium)
# -------------------------------------------------
echo "──────────────────────────────────────"
echo "  MYCELIUM (${TRANSFORM^^})"
echo "──────────────────────────────────────"

# Choose DB path and CSV based on transform type
case "$TRANSFORM" in
  splitting)
    DB_PATH="${SPLITTING_DB}"
    CSV_PATH="${SPLIT_CSV}"
    ;;
  converting)
    # Assuming a separate DB directory for converting – adjust as needed
    DB_PATH="/holly/results/bench-converting"
    CSV_PATH="${CONVERT_CSV}"
    ;;
  indexing)
    DB_PATH="/holly/results/bench-indexing"
    CSV_PATH="${INDEX_CSV}"
    ;;
  *)
    echo "ERROR: Unknown transform type '${TRANSFORM}'. Supported: splitting, converting, indexing"
    exit 1
    ;;
esac

echo "Removing old ${TRANSFORM} DB at ${DB_PATH} ..."
rm -rf "${DB_PATH}"

run_phase "${TRANSFORM}/load" "${YCSB}" \
  -P "${WORKLOAD}" \
  -db ${TRANSFORM} \
  -dbpath "${DB_PATH}" \
  -bootstrap true \
  -load true \
  -threads "${THREADS}"

run_phase "${TRANSFORM}/run" "${YCSB}" \
  -P "${WORKLOAD}" \
  -db ${TRANSFORM} \
  -dbpath "${DB_PATH}" \
  -bootstrap false \
  -throughput true \
  -runtime "${RUNTIME}" \
  -skip "${SKIP}" \
  -xputwindow "${XPUT_WINDOW}" \
  -xputfile "${CSV_PATH}" \
  -threads "${THREADS}"

echo "${TRANSFORM^} CSV appended to: ${CSV_PATH}"

echo
# -------------------------------------------------
# Cleanup – run after every execution (both phases)
# -------------------------------------------------
echo "Cleaning up temporary databases..."
rm -rf "${BASELINE_DB}" "${SPLITTING_DB}" "${DB_PATH}" 2>/dev/null || true

echo "============================================================"
if $COMPARISON; then
  echo "  Done. Results produced:"
  echo "    ${BASELINE_CSV}"
else
  echo "  Done. No baseline CSV produced (comparison disabled)."
fi
echo "    ${CSV_PATH}"
echo "============================================================"
