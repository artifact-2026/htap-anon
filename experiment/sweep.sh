#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<EOF
Usage: $0 <db_name> <workload_name> <device_name> [--cpus "1,2,4,8,16"] [--base-cpu 0]

Examples:
  $0 rocksdb A nvme0n1 --cpus "1,2,4,8,16"
  $0 rocksdb A nvme0n1 --cpus "4,6,8,10,12" --base-cpu 0
EOF
  exit 1
}

[[ $# -ge 3 ]] || usage

db_name=$1
workload_name=$2
device_name=$3
shift 3

# Defaults
cpus_csv="1,2,4,8,16"
base_cpu=0

# Parse optional args
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cpus) cpus_csv="$2"; shift 2 ;;
    --base-cpu) base_cpu="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "Unknown arg: $1" >&2; usage ;;
  esac
done

# ---- Paths & files ----
run_ts=$(date +%Y%m%d-%H%M%S)
root_outdir="runs/${db_name}_${workload_name}_${run_ts}"
mkdir -p "$root_outdir"

db_path="/data/test_results/${db_name}"
workload_file="../src/test/ycsb/workloads/test_workload${workload_name}.spec"

# ---- Sanity checks ----
for bin in iostat vmstat taskset; do
  command -v "$bin" >/dev/null || { echo "Missing dependency: $bin" >&2; exit 2; }
done
[[ -x ./src/test/ycsb/ycsb_test ]] || { echo "ycsb_test binary not found/executable" >&2; exit 2; }
[[ -f "$workload_file" ]] || { echo "Workload file not found: $workload_file" >&2; exit 2; }

# Helpers
pids=()
cleanup() {
  if (( ${#pids[@]} )); then
    kill "${pids[@]}" 2>/dev/null || true
    wait "${pids[@]}" 2>/dev/null || true
    pids=()
  fi
}

# Split CSV into bash array
IFS=',' read -r -a cpu_list <<< "$cpus_csv"

echo "CPU sweep: ${cpu_list[*]}"
echo "Output root: $root_outdir"
echo

for ncores in "${cpu_list[@]}"; do
  [[ "$ncores" =~ ^[0-9]+$ ]] || { echo "Bad core count: $ncores" >&2; exit 2; }
  (( ncores >= 1 )) || { echo "Core count must be >= 1" >&2; exit 2; }

  # Build CPU range string: base_cpu ... base_cpu+ncores-1
  end_cpu=$(( base_cpu + ncores - 1 ))
  cpu_range="${base_cpu}-${end_cpu}"

  outdir="${root_outdir}/cpu${ncores}"
  mkdir -p "$outdir"

  echo "============================================================"
  echo "Run with ncores=${ncores} (cpuset=${cpu_range})"
  echo "Logs: ${outdir}"
  echo "============================================================"

  # Start monitors for THIS run
  cleanup
  echo "Starting monitors..."
  iostat -dx -t -y "$device_name" 1 > "${outdir}/iostat.log" &
  pids+=("$!")
  vmstat -n -t 1 > "${outdir}/vmstat.log" &
  pids+=("$!")

  cpu_list_str=$(seq -s, "$base_cpu" "$end_cpu")
  mpstat -P "$cpu_list_str" 1 > "${outdir}/mpstat.log" &

  # ---- Build YCSB command ----
  # IMPORTANT: if -bootstrap true recreates/clears DB, you might not want that for every sweep point.
  # If your goal is steady-state compaction behavior, consider running a single bootstrap/load once,
  # then do sweep on the "run" phase. For now, we keep your original flags as-is.
  ycsb_cmd=( ./src/test/ycsb/ycsb_test
    -db "$db_name"
    -dbpath "$db_path"
    -P "$workload_file"
    -bootstrap false
    -threads 4*$ncores 
    -load false
    -run false
    -throughput true
    -throughputtype 2
    -runtime 300
    -table "$db_name"
  )

  echo "Starting YCSB under taskset -c ${cpu_range}:"
  printf '  %q ' taskset -c "${cpu_range}" "${ycsb_cmd[@]}"; echo

  # Run + tee output
  # Use PIPESTATUS to propagate ycsb exit code even with tee.
  set +e
  taskset -c "${cpu_range}" "${ycsb_cmd[@]}" > "${outdir}/ycsb.out" 2>&1 & ycsb_pid=$!
  
  pidstat -u -t -p "${ycsb_pid}" 1 > "${outdir}/pidstat_ycsb.log" &
  pids+=("$!")

  wait "${ycsb_pid}"
  rc=$?
  set -e

  echo "YCSB exit code: $rc" | tee "${outdir}/exit_code.txt"

  echo "Stopping monitors..."
  cleanup

  if (( rc != 0 )); then
    echo "Run failed for ncores=${ncores}. Stopping sweep." >&2
    exit "$rc"
  fi

  echo
done

echo "All runs complete."
echo "Results under: $root_outdir"

