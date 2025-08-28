#!/usr/bin/env bash
set -euo pipefail

usage() { echo "Usage: $0 <db_name> <workload_name> <device_name>" >&2; exit 1; }
[[ $# -eq 3 ]] || usage

db_name=$1
workload_name=$2
device_name=$3

# ---- Paths & files ----
run_ts=$(date +%Y%m%d-%H%M%S)
outdir="runs/${db_name}_${workload_name}_${run_ts}"
mkdir -p "$outdir"

db_path="/holly/test_results/${db_name}"
workload_file="../src/test/ycsb/workloads/test_workload${workload_name}.spec"

# ---- Sanity checks ----
for bin in iostat vmstat; do
  command -v "$bin" >/dev/null || { echo "Missing dependency: $bin" >&2; exit 2; }
done
[[ -x ./src/test/ycsb/ycsb_test ]] || { echo "ycsb_test binary not found/executable" >&2; exit 2; }
[[ -f "$workload_file" ]] || { echo "Workload file not found: $workload_file" >&2; exit 2; }

# ---- Cleanup on exit (also on error) ----
pids=()
cleanup() {
  trap - INT TERM EXIT
  if (( ${#pids[@]} )); then
    kill "${pids[@]}" 2>/dev/null || true
    wait "${pids[@]}" 2>/dev/null || true
  fi
}
trap cleanup INT TERM EXIT

echo "Starting system monitoring..."

# iostat: -d extended, -x per-device stats, -t timestamps, -y skip first (since boot) report
iostat -dx -t -y "$device_name" 1 > "${outdir}/iostat.log" &
pids+=("$!")

# vmstat: -n (don’t repeat headers), -t timestamps, 1s interval
vmstat -n -t 1 > "${outdir}/vmstat.log" &
pids+=("$!")

# ---- Build and run YCSB command (use an array to preserve quoting) ----
ycsb_cmd=( ./src/test/ycsb/ycsb_test
  -db "$db_name"
  -dbpath "$db_path"
  -P "$workload_file"
  -bootstrap true
  -threads 8
  -load true
  -run true
  -throughput false
  -table "$db_name"
)

echo "Starting YCSB:"
printf '  %q ' "${ycsb_cmd[@]}"; echo
"${ycsb_cmd[@]}" | tee "${outdir}/ycsb.out"

echo "Benchmark complete. Stopping monitors…"
cleanup

echo "Logs written to:"
echo "  ${outdir}/iostat.log"
echo "  ${outdir}/vmstat.log"
echo "  ${outdir}/ycsb.out"