#!/usr/bin/env bash
set -euo pipefail

########################################
# User-configurable settings
########################################

DB_BENCH="${DB_BENCH:-./db_bench}"

# Base directories for DB and logs
BASE_DB_DIR="${BASE_DB_DIR:-/holly/rocksdb_exp}"
BASE_WAL_DIR="${BASE_WAL_DIR:-/holly/rocksdb_wal}"
OUT_BASE="${OUT_BASE:-./results_dbbench}"

# Device to monitor with iostat, e.g. nvme0n1, sda, md0
DEVICE="${DEVICE:-nvme0n1}"

# Thread sweep
THREADS_LIST=(${THREADS_LIST:-"1 2 4 8 16 32"})

# Workload parameters
NUM_KEYS="${NUM_KEYS:-50000000}"
KEY_SIZE="${KEY_SIZE:-16}"
VALUE_SIZE="${VALUE_SIZE:-1000}"

# RocksDB parameters
WRITE_BUFFER_SIZE="${WRITE_BUFFER_SIZE:-67108864}"          # 64 MB
MAX_WRITE_BUFFER_NUMBER="${MAX_WRITE_BUFFER_NUMBER:-4}"
TARGET_FILE_SIZE_BASE="${TARGET_FILE_SIZE_BASE:-67108864}" # 64 MB
MAX_BYTES_FOR_LEVEL_BASE="${MAX_BYTES_FOR_LEVEL_BASE:-536870912}" # 512 MB
BLOCK_SIZE="${BLOCK_SIZE:-4096}"
CACHE_SIZE="${CACHE_SIZE:-0}"
BYTES_PER_SYNC="${BYTES_PER_SYNC:-1048576}"

# Sampling interval for system monitors
MONITOR_INTERVAL="${MONITOR_INTERVAL:-1}"

# Optional run label
RUN_LABEL="${RUN_LABEL:-fillrandom_compaction_characterization}"

########################################
# Helper functions
########################################

timestamp() {
  date +"%Y%m%d_%H%M%S"
}

cleanup_bg_procs() {
  local pids=("$@")
  for pid in "${pids[@]:-}"; do
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "ERROR: required command '$1' not found in PATH" >&2
    exit 1
  fi
}

########################################
# Dependency checks
########################################

require_cmd "$DB_BENCH"
require_cmd iostat
require_cmd mpstat
require_cmd vmstat
require_cmd awk
require_cmd tee

mkdir -p "$OUT_BASE"
mkdir -p "$BASE_DB_DIR"
mkdir -p "$BASE_WAL_DIR"

RUN_TS="$(timestamp)"
RUN_ROOT="${OUT_BASE}/${RUN_LABEL}_${RUN_TS}"
mkdir -p "$RUN_ROOT"

echo "Run root: $RUN_ROOT"
echo "Monitoring block device: $DEVICE"

########################################
# Record experiment metadata
########################################

cat > "${RUN_ROOT}/experiment_config.txt" <<EOF
RUN_LABEL=${RUN_LABEL}
RUN_TIMESTAMP=${RUN_TS}
DB_BENCH=${DB_BENCH}
BASE_DB_DIR=${BASE_DB_DIR}
BASE_WAL_DIR=${BASE_WAL_DIR}
OUT_BASE=${OUT_BASE}
DEVICE=${DEVICE}
THREADS_LIST=${THREADS_LIST[*]}
NUM_KEYS=${NUM_KEYS}
KEY_SIZE=${KEY_SIZE}
VALUE_SIZE=${VALUE_SIZE}
WRITE_BUFFER_SIZE=${WRITE_BUFFER_SIZE}
MAX_WRITE_BUFFER_NUMBER=${MAX_WRITE_BUFFER_NUMBER}
TARGET_FILE_SIZE_BASE=${TARGET_FILE_SIZE_BASE}
MAX_BYTES_FOR_LEVEL_BASE=${MAX_BYTES_FOR_LEVEL_BASE}
BLOCK_SIZE=${BLOCK_SIZE}
CACHE_SIZE=${CACHE_SIZE}
BYTES_PER_SYNC=${BYTES_PER_SYNC}
MONITOR_INTERVAL=${MONITOR_INTERVAL}
EOF

uname -a > "${RUN_ROOT}/uname.txt" 2>&1 || true
lscpu > "${RUN_ROOT}/lscpu.txt" 2>&1 || true
lsblk > "${RUN_ROOT}/lsblk.txt" 2>&1 || true
df -h > "${RUN_ROOT}/df_h.txt" 2>&1 || true
mount > "${RUN_ROOT}/mount.txt" 2>&1 || true

########################################
# Main sweep
########################################

for rep in 1, 2, 3; do
for t in "${THREADS_LIST[@]}"; do
  RUN_DIR="${RUN_ROOT}/rep_${rep}/threads_${t}"
  DB_DIR="${BASE_DB_DIR}/rep_${rep}/db_threads_${t}"
  WAL_DIR="${BASE_WAL_DIR}/rep_${rep}/wal_threads_${t}"

  mkdir -p "$RUN_DIR"

  echo
  echo "============================================================"
  echo "Starting run for threads=${t}"
  echo "Run dir: ${RUN_DIR}"
  echo "DB dir:  ${DB_DIR}"
  echo "WAL dir: ${WAL_DIR}"
  echo "============================================================"

  # Clean old DB/WAL state so each run starts fresh
  rm -rf "$DB_DIR" "$WAL_DIR"
  mkdir -p "$DB_DIR" "$WAL_DIR"

  # Monitor outputs
  IOSTAT_OUT="${RUN_DIR}/iostat.txt"
  MPSTAT_OUT="${RUN_DIR}/mpstat.txt"
  VMSTAT_OUT="${RUN_DIR}/vmstat.txt"
  DBBENCH_OUT="${RUN_DIR}/db_bench.txt"
  TIME_OUT="${RUN_DIR}/time.txt"

  BG_PIDS=()

  # Start system monitors
  # -y skips first iostat report since boot
  iostat -y -x "${MONITOR_INTERVAL}" "${DEVICE}" > "$IOSTAT_OUT" 2>&1 &
  BG_PIDS+=($!)

  mpstat -P ALL "${MONITOR_INTERVAL}" > "$MPSTAT_OUT" 2>&1 &
  BG_PIDS+=($!)

  vmstat "${MONITOR_INTERVAL}" > "$VMSTAT_OUT" 2>&1 &
  BG_PIDS+=($!)

  START_TS="$(date +%s)"

  # Run db_bench
  {
    /usr/bin/time -v stdbuf -oL -eL "$DB_BENCH" \
      --benchmarks="fillrandom,stats" \
      --db="$DB_DIR" \
      --wal_dir="$WAL_DIR" \
      --num="$NUM_KEYS" \
      --threads="$t" \
      --key_size="$KEY_SIZE" \
      --value_size="$VALUE_SIZE" \
      --block_size="$BLOCK_SIZE" \
      --cache_size="$CACHE_SIZE" \
      --write_buffer_size="$WRITE_BUFFER_SIZE" \
      --max_write_buffer_number="$MAX_WRITE_BUFFER_NUMBER" \
      --target_file_size_base="$TARGET_FILE_SIZE_BASE" \
      --max_bytes_for_level_base="$MAX_BYTES_FOR_LEVEL_BASE" \
      --compression_type=none \
      --statistics \
      --stats_interval_seconds=10 \
      --histogram \
      --bytes_per_sync="$BYTES_PER_SYNC" \
      --report_bg_io_stats=true \
      --disable_auto_compactions=false
  } > "$DBBENCH_OUT" 2> "$TIME_OUT" || true

  END_TS="$(date +%s)"
  ELAPSED=$((END_TS - START_TS))

  # Stop monitors
  cleanup_bg_procs "${BG_PIDS[@]}"

  cat > "${RUN_DIR}/run_meta.txt" <<EOF
threads=${t}
rep=${rep}
start_unix=${START_TS}
end_unix=${END_TS}
elapsed_seconds=${ELAPSED}
db_dir=${DB_DIR}
wal_dir=${WAL_DIR}
EOF

  echo "Finished threads=${t} in ${ELAPSED}s"

  # Remove DB contents to save space
  rm -rf "$DB_DIR" "$WAL_DIR"

  sync
  echo 3 | sudo tee /proc/sys/vm/drop_caches

done
done

echo
echo "All runs finished."
echo "Results stored under: $RUN_ROOT"
