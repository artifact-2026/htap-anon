#!/usr/bin/env bash
# run_mycelium_docker.sh – Run run_mycelium.sh inside a Docker container
#                          with a CPU-core limit for controlled benchmarking.
#
# Usage:
#   ./run_mycelium_docker.sh --cpus <N> <BUILD_DIR> [RUNTIME_SECS] [THREADS] [OUTPUT_DIR] \
#                            [--comparison [true|false]] [--transform <type>]
#
# Arguments:
#   --cpus <N>   Number of CPU cores to allocate to the container (e.g. 2, 4, 0.5).
#                This maps directly to `docker run --cpus`.
#   All remaining arguments are forwarded verbatim to run_mycelium.sh.
#
# Examples:
#   # 4 cores, 120 s run, 8 threads, results in /tmp/bench-out, splitting transform
#   ./run_mycelium_docker.sh --cpus 4 /path/to/build 120 8 /tmp/bench-out --transform splitting
#
#   # 2 cores with baseline comparison
#   ./run_mycelium_docker.sh --cpus 2 /path/to/build 300 16 /tmp/bench-out --comparison --transform converting

set -euo pipefail

IMAGE_NAME="mycelium-bench"
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# -------------------------------------------------
# Parse --cpus; collect everything else in two buckets:
#   POS_ARGS  – positional args (BUILD_DIR, RUNTIME, THREADS, OUTPUT_DIR)
#   FLAGS     – --comparison / --transform flags to forward as-is
# -------------------------------------------------
CPUS=""
POS_ARGS=()
FLAGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cpus)
      CPUS="${2:?--cpus requires a value}"
      shift 2
      ;;
    --comparison)
      if [[ $# -gt 1 && ( "$2" == "true" || "$2" == "false" ) ]]; then
        FLAGS+=("$1" "$2"); shift 2
      else
        FLAGS+=("$1"); shift
      fi
      ;;
    --transform)
      FLAGS+=("$1" "${2:?--transform requires a value}"); shift 2
      ;;
    *)
      POS_ARGS+=("$1"); shift
      ;;
  esac
done

if [[ -z "$CPUS" ]]; then
  echo "ERROR: --cpus <N> is required"
  echo "Usage: $0 --cpus <N> <BUILD_DIR> [RUNTIME_SECS] [THREADS] [OUTPUT_DIR] [--comparison] [--transform <type>]"
  exit 1
fi

if [[ ${#POS_ARGS[@]} -lt 1 ]]; then
  echo "ERROR: BUILD_DIR is required"
  exit 1
fi

# -------------------------------------------------
# Resolve host paths to absolute
# -------------------------------------------------
BUILD_DIR=$(realpath "${POS_ARGS[0]}")
RUNTIME=${POS_ARGS[1]:-300}
THREADS=${POS_ARGS[2]:-16}
OUTPUT_DIR=${POS_ARGS[3]:-./results}

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR=$(realpath "$OUTPUT_DIR")

# Container-internal paths (fixed; volumes are mounted to these)
C_BUILD=/bench/build
C_YCSB=/bench/ycsb
C_RESULTS=/bench/results

# -------------------------------------------------
# Build the runtime image (cached after first run)
# -------------------------------------------------
echo "==> Building Docker image '${IMAGE_NAME}' (cached if unchanged)..."
docker build -q \
  -t "$IMAGE_NAME" \
  -f "${SCRIPT_DIR}/Dockerfile.bench" \
  "${SCRIPT_DIR}"

# -------------------------------------------------
# Run the benchmark
# -------------------------------------------------
echo "==> Launching benchmark:"
echo "    CPUs:       ${CPUS}"
echo "    BUILD_DIR:  ${BUILD_DIR}"
echo "    RUNTIME:    ${RUNTIME}s"
echo "    THREADS:    ${THREADS}"
echo "    OUTPUT_DIR: ${OUTPUT_DIR}"
[[ ${#FLAGS[@]} -gt 0 ]] && echo "    FLAGS:      ${FLAGS[*]}"
echo

docker run --rm \
  --cpus="${CPUS}" \
  -v "${BUILD_DIR}:${C_BUILD}:ro" \
  -v "${SCRIPT_DIR}:${C_YCSB}:ro" \
  -v "${OUTPUT_DIR}:${C_RESULTS}" \
  "${IMAGE_NAME}" \
  bash "${C_YCSB}/run_mycelium.sh" \
    "${C_BUILD}" \
    "${RUNTIME}" \
    "${THREADS}" \
    "${C_RESULTS}" \
    "${FLAGS[@]+"${FLAGS[@]}"}"
