#!/bin/bash
set -euo pipefail

INPUT_DIR="$1"
OUTPUT_DIR="$2"
LOG_DIR="$3"

mkdir -p "$OUTPUT_DIR" "$LOG_DIR"

# Fixed binary locations
SSTDUMP_BIN="./src/mycelium/tools/sst_dump"
INDEX_BIN="../src/test/transformers/scripts/build_prefix_index"

# Start monitors
echo "[*] Starting monitors..."
vmstat 1 > "$LOG_DIR/vmstat.log" &
VMSTAT_PID=$!
iostat -x 1 > "$LOG_DIR/iostat.log" &
IOSTAT_PID=$!

# Process each SST file in INPUT_DIR
echo "[*] Processing SST files from $INPUT_DIR..."
for sst_file in "$INPUT_DIR"/*.sst; do
    base_name=$(basename "$sst_file" .sst)

    outpath="$OUTPUT_DIR/${base_name}_index.sst"
    stdout_log="$LOG_DIR/${base_name}_stdout.log"
    stderr_log="$LOG_DIR/${base_name}_stderr.log"

    echo "    - Processing $sst_file" for indexing
    (
      "$SSTDUMP_BIN" --command=scan --file="$sst_file" --output_hex \
        | awk -F'=> ' '/=>/ && NF==2 {print $2}' \
        | "$INDEX_BIN" --build-sst="$outpath"
    ) >"$stdout_log" 2>"$stderr_log"
done

# Stop monitors
echo "[*] Stopping monitors..."
kill $VMSTAT_PID $IOSTAT_PID

echo "[*] Done."