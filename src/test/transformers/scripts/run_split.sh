#!/bin/bash
set -e

# ---------------------------
# Usage check
# ---------------------------
if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <input_dir> <output_dir> <log_dir>"
    exit 1
fi

INPUT_DIR="$1"
OUTPUT_DIR="$2"
LOG_DIR="$3"

mkdir -p "$OUTPUT_DIR"
mkdir -p "$LOG_DIR"

DEVICE=$(df "$OUTPUT_DIR" | tail -1 | awk '{print $1}')

iostat -dx $DEVICE 1 > iostat_log.txt &
IOSTAT_PID=$!
vmstat 1 > vmstat_log.txt &
VMSTAT_PID=$!

for FILEPATH in "$INPUT_DIR"/*.sst; do
    FILENAME=$(basename "$FILEPATH")
    BASENAME="${FILENAME%.*}"  # remove .sst extension

    echo "Processing: $FILENAME"

    # Create per-file output and log directories
    OUT_PATH="$OUTPUT_DIR/$BASENAME"
    LOG_PATH="$LOG_DIR/$BASENAME"
    mkdir -p "$OUT_PATH" "$LOG_PATH"


    # Run transformation
    ./bin/split_column_groups "$FILEPATH" "$OUT_PATH" > "$LOG_PATH/stdout.log" 2> "$LOG_PATH/stderr.log"
done

set +e
kill "$IOSTAT_PID" 2>/dev/null || true
kill "$VMSTAT_PID" 2>/dev/null || true
wait "$IOSTAT_PID" 2>/dev/null || true
wait "$VMSTAT_PID" 2>/dev/null || true
set -e

echo "--- Analysis for splitting transformation ---"
./analyze_baseline.sh
echo
