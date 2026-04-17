#!/usr/bin/env bash
# =============================================================================
# identify_bottleneck.sh — Post-saturation bottleneck classification
#
# Given the saturation knee (KNEE_THREADS), determines whether the system is
# CPU-bound, IO-bound, or both by probing with progressively heavier
# interference and measuring throughput degradation.
#
# Algorithm
# ─────────
#   found_bottleneck = false
#   cpu_bound = false ; io_bound = false
#   Workers = 1 ; Intensity = 1
#
#   while !found_bottleneck:
#       if sweep_cpu(Intensity) drops xput > DROP_THRESHOLD_PCT%:
#           found_bottleneck = true ; cpu_bound = true
#       if sweep_io(Intensity) drops xput > DROP_THRESHOLD_PCT%:
#           found_bottleneck = true ; io_bound = true
#       if Intensity >= MAX_INTENSITY: break   # safety exit (inconclusive)
#       Intensity += INTENSITY_STEP
#
#   return cpu_bound, io_bound
#
# Key properties:
#   • Both probes run every iteration — both can trigger simultaneously.
#   • CPU target  = Intensity × CPU_UNIT_PCT % of ALL system cores.
#   • IO  target  = Intensity × IO_UNIT_PCT  % of device write bandwidth.
#   • Each probe loads a fresh DB so LSM state is identical across comparisons.
#   • Verdict: "cpu_bound", "io_bound", "cpu_and_io_bound", or "inconclusive".
#
# Usage (from project BUILD directory):
#   KNEE_THREADS=16 \
#   WORKLOAD_SPEC=../src/test/ycsb/workloads/workloada.spec \
#   IO_SCRATCH_DIR=/path/on/same/device/as/rocksdb \
#   bash ../experiment/saturation/identify_bottleneck.sh
#
# ── Key knobs ─────────────────────────────────────────────────────────────────
#   KNEE_THREADS      Fixed YCSB thread count (the saturation knee).
#   KNEE_XPUT         Peak throughput at KNEE_THREADS from the saturation sweep
#                     (ops/s). REQUIRED — read from summary.csv xput_mean column.
#   PROBE_RUNTIME_SECS  Wall-clock seconds per probe window (default 300).
#   PROBE_WARMUP_S    Warmup seconds excluded from throughput avg (default 60).
#   DROP_THRESHOLD_PCT  % drop vs KNEE_XPUT to declare a bottleneck (default 3).
#   MAX_INTENSITY     Intensity ceiling; declared inconclusive if exceeded.
#   INTENSITY_STEP    Step between intensity levels (default 1).
#   CPU_UNIT_PCT      % of ALL system cores per intensity unit (default 10).
#                     cpu_target = Intensity × CPU_UNIT_PCT % of NCORES.
#   IO_UNIT_PCT       % of max device write BW per intensity unit (default 10).
#                     io_target  = Intensity × IO_UNIT_PCT % of MAX_IO_BW.
#   IO_BLOCK_SIZE     Bytes per O_DIRECT write call (must be multiple of 512).
#   IO_DATA_SIZE_MB   Scratch file size in MiB.
#   IO_SCRATCH_DIR    Directory for IO scratch files — MUST be on the same
#                     device as the RocksDB data for real I/O contention.
#
# ── Output ────────────────────────────────────────────────────────────────────
# <OUTPUT_DIR>/
#   cpu_probe_w<W>_i<I>/    CPU probe at (Workers=W, Intensity=I)
#   io_probe_w<W>_i<I>/     IO probe  at (Workers=W, Intensity=I)
#   results.csv             one row per probe: xput, drop%, triggered, verdict
# =============================================================================

set -euo pipefail

BINARY="${BINARY:-./src/test/ycsb/ycsb_test}"
OUTPUT_DIR="${OUTPUT_DIR:-./bottleneck_$(date +%Y%m%d_%H%M%S)}"

# ── Workload ──────────────────────────────────────────────────────────────────
WORKLOAD_SPEC="${WORKLOAD_SPEC:-}"
WORKLOAD_LABEL="${WORKLOAD_LABEL:-}"

# ── Experiment knobs ──────────────────────────────────────────────────────────
KNEE_THREADS="${KNEE_THREADS:-16}"
RECORD_COUNT="${RECORD_COUNT:-10000000}"
DISK_DEVICE="${DISK_DEVICE:-nvme0n1}"
DROP_CACHES="${DROP_CACHES:-true}"

# Per-probe timing.
PROBE_RUNTIME_SECS="${PROBE_RUNTIME_SECS:-300}"
PROBE_WARMUP_S="${PROBE_WARMUP_S:-60}"

# Bottleneck decision.
DROP_THRESHOLD_PCT="${DROP_THRESHOLD_PCT:-3}"
# Peak throughput from the saturation sweep (ops/s).  Required — no default.
# Pass the value from summary.csv at KNEE_THREADS.
KNEE_XPUT="${KNEE_XPUT:-0}"

# Sweep: intensity steps from 1 up by INTENSITY_STEP to MAX_INTENSITY.
# At intensity=I: CPU uses I × CPU_UNIT_PCT% of all cores,
#                 IO  uses I × IO_UNIT_PCT%  of device bandwidth.
# Declared inconclusive if MAX_INTENSITY reached without triggering.
MAX_INTENSITY="${MAX_INTENSITY:-10}"
INTENSITY_STEP="${INTENSITY_STEP:-1}"

# CPU interference per intensity level:
#   cpu_target_pct = intensity × CPU_UNIT_PCT  (% of ALL system cores)
#   Achieved by floor(cpu_target_pct × NCORES / 100) fully-saturated iBench
#   threads + 1 partial thread at the fractional remainder.
# iBench_cpu workers run Murmur3 transform loops at rate=0 (full saturation)
# or rate=R (partial, calibrated via SATURATE_RATE_PER_THREAD).
CPU_UNIT_PCT="${CPU_UNIT_PCT:-10}"
WORKER_ROUNDS_MULTIPLIER="${WORKER_ROUNDS_MULTIPLIER:-8}"  # × 1024 rounds/transform

# IO interference per intensity level:
#   io_target_mbs = intensity × IO_UNIT_PCT / 100 × MAX_IO_BW_MBS_PER_WORKER
#   Achieved by 1 IO worker thread writing at io_target_mbs MB/s (rate-limited).
#   At intensity=MAX_INTENSITY the worker saturates the device (rate=0).
IO_UNIT_PCT="${IO_UNIT_PCT:-10}"                   # % of max single-thread IO BW per intensity unit
IO_BLOCK_SIZE="${IO_BLOCK_SIZE:-524288}"            # bytes per O_DIRECT write; 512 KiB hits sequential BW
IO_DATA_SIZE_MB="${IO_DATA_SIZE_MB:-2048}"          # scratch file size per IO worker (MiB)
IO_SCRATCH_DIR="${IO_SCRATCH_DIR:-}"               # must be on same device as RocksDB

# =============================================================================
# Internals
# =============================================================================

SAT_TMPDIR="${SAT_TMPDIR:-/holly/htap/build}"

MONITOR_SCRIPT=""
IBENCH_CPU_BINARY=""
IO_WORKER_SCRIPT=""
TMPSPEC=""
MONITOR_PID=""
IO_WORKER_PID=""
NCORES=1                       # detected at startup by get_ncores()
MAX_IO_BW_MBS_PER_WORKER=""   # calibrated at startup
declare -a CPU_WORKER_PIDS=()
declare -a IO_SCRATCH_FILES=()


log() { echo "[$(date '+%H:%M:%S')] $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
sep() { echo ""; log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"; }

cleanup() {
    [[ -n "${MONITOR_PID:-}" ]] && kill "$MONITOR_PID" 2>/dev/null || true
    local pid
    for pid in "${CPU_WORKER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    [[ -n "${IO_WORKER_PID:-}" ]] && kill "$IO_WORKER_PID" 2>/dev/null || true
    [[ -n "${MONITOR_SCRIPT:-}" ]]   && rm -f "$MONITOR_SCRIPT"
    [[ -n "${IBENCH_CPU_BINARY:-}" ]] && rm -f "$IBENCH_CPU_BINARY"
    [[ -n "${IO_WORKER_SCRIPT:-}" ]]  && rm -f "$IO_WORKER_SCRIPT"
    [[ -n "${TMPSPEC:-}" ]]           && rm -f "$TMPSPEC"
    # Remove pre-allocated IO scratch files registered during the run.
    local f
    for f in "${IO_SCRATCH_FILES[@]:-}"; do
        [[ -f "$f" ]] && rm -f "$f" || true
    done
}
trap cleanup EXIT

# ERR trap: print the failing command and line number before set -e kills us.
on_error() {
    local exit_code=$? line="$1" cmd="$2"
    echo "" >&2
    echo "[ERROR] Script aborted at line $line (exit=$exit_code)" >&2
    echo "[ERROR] Command: $cmd" >&2
    echo "[ERROR] Check above output for details." >&2
}
trap 'on_error "$LINENO" "$BASH_COMMAND"' ERR

# ── Pre-flight ────────────────────────────────────────────────────────────────

check_prereqs() {
    [[ -x "$BINARY" ]]        || die "ycsb_test binary not found at '$BINARY'."
    [[ -n "$WORKLOAD_SPEC" ]] || die "WORKLOAD_SPEC is not set."
    [[ -f "$WORKLOAD_SPEC" ]] || die "WORKLOAD_SPEC not found: $WORKLOAD_SPEC"
    command -v python3 >/dev/null 2>&1 || die "python3 required"
    if [[ -z "$WORKLOAD_LABEL" ]]; then
        WORKLOAD_LABEL="$(basename "$WORKLOAD_SPEC" .spec)"
    fi
    if [[ -z "$IO_SCRATCH_DIR" ]]; then
        IO_SCRATCH_DIR="$OUTPUT_DIR/io_scratch"
        log "  IO_SCRATCH_DIR not set — defaulting to $IO_SCRATCH_DIR"
        log "  ⚠  For meaningful IO contention this must be on the SAME device"
        log "     as the RocksDB data directory. Set IO_SCRATCH_DIR explicitly."
    fi
    mkdir -p "$IO_SCRATCH_DIR"
    if ! awk '{print $3}' /proc/diskstats 2>/dev/null | grep -qx "$DISK_DEVICE"; then
        log "WARNING: '$DISK_DEVICE' not found in /proc/diskstats — disk metrics will be zero."
    fi
    (( PROBE_RUNTIME_SECS > PROBE_WARMUP_S )) || \
        die "PROBE_RUNTIME_SECS ($PROBE_RUNTIME_SECS) must be > PROBE_WARMUP_S ($PROBE_WARMUP_S)"
    (( IO_BLOCK_SIZE % 512 == 0 )) || \
        die "IO_BLOCK_SIZE ($IO_BLOCK_SIZE) must be a multiple of 512 for O_DIRECT"
    (( MAX_INTENSITY >= 2 )) || \
        die "MAX_INTENSITY must be >= 2 (inner loop runs Intensity 1..MAX_INTENSITY-1)"
    python3 -c "import sys; v=float('${KNEE_XPUT}'); sys.exit(0 if v > 0 else 1)" \
        || die "KNEE_XPUT must be set to the peak throughput (ops/s) from the saturation sweep."
}

# ── Per-second system monitor (identical to saturation_sweep.sh) ──────────────

setup_monitor_script() {
    MONITOR_SCRIPT=$(mktemp "${SAT_TMPDIR}/botdet_mon_XXXXX.py")
    cat > "$MONITOR_SCRIPT" << 'PYMON'
#!/usr/bin/env python3
"""Per-second CPU, disk I/O, and memory monitor."""
import sys, time, csv

outfile     = sys.argv[1]
disk_device = sys.argv[2] if len(sys.argv) > 2 else ""

def read_cpu():
    with open('/proc/stat') as f:
        parts = f.readline().split()
    return [int(x) for x in parts[1:9]]

def read_disk(dev):
    if not dev: return 0, 0, 0, 0
    with open('/proc/diskstats') as f:
        for line in f:
            p = line.split()
            if p[2] == dev:
                return int(p[5]), int(p[9]), int(p[3]), int(p[7])
    return 0, 0, 0, 0

def read_mem_mib():
    info = {}
    with open('/proc/meminfo') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                info[parts[0].rstrip(':')] = int(parts[1])
    total = info.get('MemTotal', 0) / 1024
    avail = info.get('MemAvailable', 0) / 1024
    return total, avail, total - avail

prev_cpu = read_cpu()
prev_rd, prev_wr, prev_rc, prev_wc = read_disk(disk_device)
prev_t   = time.time()

with open(outfile, 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['timestamp_s',
                'cpu_user_pct', 'cpu_sys_pct', 'cpu_iowait_pct', 'cpu_idle_pct',
                'disk_read_mbs', 'disk_write_mbs',
                'disk_read_iops', 'disk_write_iops',
                'mem_total_mib', 'mem_used_mib', 'mem_avail_mib'])
    fh.flush()
    while True:
        time.sleep(1)
        now = time.time()
        cur_cpu = read_cpu()
        cur_rd, cur_wr, cur_rc, cur_wc = read_disk(disk_device)
        m_total, m_avail, m_used = read_mem_mib()
        dt    = (now - prev_t) or 1
        delta = [cur_cpu[i] - prev_cpu[i] for i in range(8)]
        total = sum(delta) or 1
        w.writerow([int(now),
                    f'{100*delta[0]/total:.2f}', f'{100*delta[2]/total:.2f}',
                    f'{100*delta[4]/total:.2f}', f'{100*delta[3]/total:.2f}',
                    f'{(cur_rd-prev_rd)*512/1024/1024/dt:.3f}',
                    f'{(cur_wr-prev_wr)*512/1024/1024/dt:.3f}',
                    f'{(cur_rc-prev_rc)/dt:.2f}', f'{(cur_wc-prev_wc)/dt:.2f}',
                    f'{m_total:.1f}', f'{m_used:.1f}', f'{m_avail:.1f}'])
        fh.flush()
        prev_cpu = cur_cpu
        prev_rd, prev_wr, prev_rc, prev_wc = cur_rd, cur_wr, cur_rc, cur_wc
        prev_t   = now
PYMON
}

start_monitor() { python3 "$MONITOR_SCRIPT" "$1" "$DISK_DEVICE" & MONITOR_PID=$!; }
stop_monitor() {
    set +e
    if [[ -n "${MONITOR_PID:-}" ]]; then
        kill "$MONITOR_PID" 2>/dev/null
        wait "$MONITOR_PID" 2>/dev/null
        MONITOR_PID=""
    fi
    set -e
}

# ── Compiled iBench_cpu worker (same binary as cpu_slack_sweep_v2.sh) ─────────
#
# iBench_cpu runs Murmur3-style 64-bit integer mix rounds on n_threads cores.
# Duty cycle control:
#   rate=0             → full busy-loop (100% of the core)
#   rate=R transforms/s → worker sleeps between transforms via clock_nanosleep,
#                         achieving approximately duty% utilization where
#                         duty ≈ (compute_ns_per_xfm × R) / 1e9  × 100.
#
# For the bottleneck sweep we convert cpu_duty_pct to a transforms/s rate:
#   duty_pct=100  → rate=0 (saturate)
#   duty_pct<100  → rate= round(SATURATE_RATE × duty_pct / 100)
#
# SATURATE_RATE is the max transforms/s each thread can sustain (calibrated at
# startup with a 2-second warm-up).  This ensures the mapping is portable
# across machines without hard-coding a magic constant.

get_ncores() {
    NCORES=$(nproc 2>/dev/null) \
        || NCORES=$(python3 -c "import os; print(os.cpu_count())" 2>/dev/null) \
        || NCORES=1
    log "  System CPU cores: $NCORES"
}

compile_ibench_cpu() {
    local src_cc; src_cc="$(dirname "$0")/../slack/iBench_cpu.cc"
    [[ -f "$src_cc" ]] || die "iBench_cpu.cc not found at '$src_cc'"
    IBENCH_CPU_BINARY=$(mktemp "${SAT_TMPDIR}/botdet_cpu_XXXXX")
    log "Compiling iBench_cpu.cc → $IBENCH_CPU_BINARY"
    g++ -O2 -o "$IBENCH_CPU_BINARY" "$src_cc" -lpthread \
        || die "Failed to compile iBench_cpu.cc. Need g++ (no special flags required)."
    chmod +x "$IBENCH_CPU_BINARY"
}

# Calibrate the saturate rate: run 1 thread for 2 s at rate=0 and measure
# actual transforms/s.  Stored in SATURATE_RATE_PER_THREAD (global).
SATURATE_RATE_PER_THREAD=""
calibrate_cpu_rate() {
    # If the user already provided an override, use it directly.
    if [[ -n "${IBENCH_SATURATE_RATE:-}" ]]; then
        SATURATE_RATE_PER_THREAD="$IBENCH_SATURATE_RATE"
        log "Calibration: using IBENCH_SATURATE_RATE override: $SATURATE_RATE_PER_THREAD xfm/s"
        return
    fi

    log "Calibrating iBench_cpu saturate rate (2 s full-blast, 1 thread) ..."
    local rounds=$(( 1024 * WORKER_ROUNDS_MULTIPLIER ))
    # Run for 2 s at rate=0 (full busy-loop). Parse "transforms=N" from output.
    local calib_out
    calib_out=$("$IBENCH_CPU_BINARY" 2 1 0 "$rounds" 2>&1) || true

    SATURATE_RATE_PER_THREAD=$(python3 -c "
import re, sys
m = re.search(r'transforms=(\d+).*duration=(\d+)s', '''$calib_out''')
if m:
    xfm, dur = int(m.group(1)), int(m.group(2))
    print(max(1, xfm // max(1, dur)))
else:
    sys.exit(1)
" 2>/dev/null) || {
        SATURATE_RATE_PER_THREAD=50000
        log "  WARNING: calibration parse failed; defaulting to $SATURATE_RATE_PER_THREAD xfm/s"
        log "    Set IBENCH_SATURATE_RATE=<N> to override."
        return
    }

    log "  Saturate rate: $SATURATE_RATE_PER_THREAD xfm/s per thread (rounds=$rounds)"
    log "  Duty mapping: 10% duty → rate=$(( SATURATE_RATE_PER_THREAD / 10 )) xfm/s per worker"
}

# duty_pct_to_rate <duty_pct>  →  echo transforms/s (0 = saturate)
duty_pct_to_rate() {
    local duty_pct="$1"
    if (( duty_pct >= 100 )); then
        echo 0   # full busy-loop
    else
        python3 -c "print(max(1, round($SATURATE_RATE_PER_THREAD * $duty_pct / 100)))"
    fi
}

# start_cpu_workers <n_full> <partial_pct> <duration_s>
#
# n_full      — number of threads to run at full saturation (rate=0)
# partial_pct — duty-cycle % (0-99) for one additional thread; 0 = skip
#
# Together these achieve:
#   (n_full + partial_pct/100) / NCORES × 100 % of total CPU capacity.
start_cpu_workers() {
    local n_full="$1" partial_pct="$2" duration="$3"
    CPU_WORKER_PIDS=()
    if (( n_full <= 0 && partial_pct <= 0 )); then
        log "    iBench_cpu: target < 0.5% of 1 core — skipping CPU interference."
        return
    fi
    local rounds=$(( 1024 * WORKER_ROUNDS_MULTIPLIER ))
    if (( n_full > 0 )); then
        log "    iBench_cpu: $n_full fully-saturated thread(s) for ${duration}s"
        "$IBENCH_CPU_BINARY" "$duration" "$n_full" 0 "$rounds" &
        CPU_WORKER_PIDS+=($!)
    fi
    if (( partial_pct > 0 )); then
        local partial_rate; partial_rate=$(duty_pct_to_rate "$partial_pct")
        log "    iBench_cpu: 1 partial thread @ ${partial_pct}% duty (rate=${partial_rate} xfm/s) for ${duration}s"
        "$IBENCH_CPU_BINARY" "$duration" 1 "$partial_rate" "$rounds" &
        CPU_WORKER_PIDS+=($!)
    fi
}

stop_cpu_workers() {
    local pid
    set +e
    for pid in "${CPU_WORKER_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    set -e
    CPU_WORKER_PIDS=()
}

# ── IO interference worker (embedded Python, O_DIRECT) ───────────────────────
# Mirrors the worker from disk_io_slack_sweep.sh exactly.

setup_io_worker_script() {
    IO_WORKER_SCRIPT=$(mktemp "${SAT_TMPDIR}/botdet_io_XXXXX.py")
    cat > "$IO_WORKER_SCRIPT" << 'PYIO'
#!/usr/bin/env python3
"""
O_DIRECT write-only interference worker.
Usage: python3 <script> <data_file> <rate_mbs> <block_size> <duration_s>
  rate_mbs=0 → write as fast as possible (saturate device bandwidth)
"""
import sys, os, time, ctypes

data_file  = sys.argv[1]
rate_mbs   = float(sys.argv[2])
block_size = int(sys.argv[3])
duration_s = int(sys.argv[4])

assert block_size % 512 == 0, f"block_size must be multiple of 512, got {block_size}"
file_size = os.path.getsize(data_file)
assert file_size > 0, f"data file is empty: {data_file}"

# Aligned write buffer for O_DIRECT.
# IMPORTANT: bytes(ctypes_array) copies to a new, potentially unaligned Python
# bytes object.  O_DIRECT requires 512-byte alignment; unaligned buffers cause
# every write() to fail with EINVAL (silently caught), yielding 0 bytes written.
# Fix: allocate via posix_memalign and keep as a ctypes array so os.write()
# sees the actual aligned pointer.
_libc    = ctypes.CDLL(None)
_ptr     = ctypes.c_void_p()
_align   = max(4096, block_size)
if _libc.posix_memalign(ctypes.byref(_ptr), _align, block_size) != 0:
    raise MemoryError("posix_memalign failed")
write_buf = (ctypes.c_char * block_size).from_address(_ptr.value)
ctypes.memset(_ptr, 0xAB, block_size)

bytes_per_sec = rate_mbs * 1024.0 * 1024.0

try:
    fd = os.open(data_file, os.O_WRONLY | os.O_DIRECT)
except (OSError, AttributeError):
    fd = os.open(data_file, os.O_WRONLY)

end_time    = time.time() + duration_s
pos         = 0
total_bytes = 0

def write_one():
    """Write one block, wrapping the file. Returns bytes written (0 on error)."""
    global pos
    if pos + block_size > file_size:
        pos = 0
        os.lseek(fd, 0, os.SEEK_SET)
    try:
        n = os.write(fd, write_buf)
        pos += n
        return n
    except OSError:
        pos = 0
        os.lseek(fd, 0, os.SEEK_SET)
        return 0

try:
    if bytes_per_sec <= 0:
        # rate=0: full saturation — write as fast as possible.
        while time.time() < end_time:
            total_bytes += write_one()
    else:
        # Window-based rate limiter.
        #
        # WHY NOT per-write sleep?
        #   Python time.sleep() has ~1 ms minimum resolution on Linux.
        #   At 185 MB/s with 4 KB blocks the required inter-write gap is
        #   ~21 µs.  sleep(21µs) actually sleeps ~1 ms → actual rate ≈ 4 MB/s
        #   (46× too slow).
        #
        # FIX — 20 ms window:
        #   Write enough blocks to fill a 20 ms slot as fast as possible,
        #   then sleep for the remainder.  20 ms >> 1 ms sleep floor, so
        #   the sleep is accurate to ±1 ms → rate error < 5%.
        WINDOW_S       = 0.020
        bytes_per_win  = bytes_per_sec * WINDOW_S
        blocks_per_win = max(1, int(bytes_per_win / block_size))

        while time.time() < end_time:
            t_win = time.monotonic()
            for _ in range(blocks_per_win):
                if time.time() >= end_time:
                    break
                total_bytes += write_one()
            sleep_s = WINDOW_S - (time.monotonic() - t_win)
            if sleep_s > 0.001:
                time.sleep(sleep_s)
finally:
    os.close(fd)

actual_mbs = total_bytes / 1024.0 / 1024.0 / max(1, duration_s)
print(f"[iBench_io] {data_file}: target={rate_mbs:.1f} MB/s "
      f"actual={actual_mbs:.1f} MB/s total={total_bytes/1024/1024:.0f} MiB",
      flush=True)
PYIO
}

# Preallocate a scratch file for one IO worker; registers path for EXIT cleanup.
alloc_io_file() {
    local path="$1" size_mb="$2"
    log "  Pre-allocating ${size_mb} MiB IO scratch: $path"
    dd if=/dev/zero of="$path" bs=1M count="$size_mb" conv=fdatasync 2>/dev/null
    IO_SCRATCH_FILES+=("$path")
}

# calibrate_io_rate: measure the saturate write BW of a single IO worker
# on IO_SCRATCH_DIR for 2 seconds at rate=0. Stores result in
# MAX_IO_BW_MBS_PER_WORKER (global).
calibrate_io_rate() {
    if [[ -n "${IBENCH_IO_SATURATE_MBS:-}" ]]; then
        MAX_IO_BW_MBS_PER_WORKER="$IBENCH_IO_SATURATE_MBS"
        log "Calibration: using IBENCH_IO_SATURATE_MBS override: ${MAX_IO_BW_MBS_PER_WORKER} MB/s"
        return
    fi

    log "Calibrating IO worker saturate BW (2 s full-blast on $IO_SCRATCH_DIR) ..."
    local calib_file="$IO_SCRATCH_DIR/ibench_io_calib.dat"
    # Pre-allocate a small (128 MiB) calibration file.
    dd if=/dev/zero of="$calib_file" bs=1M count=512 conv=fdatasync 2>/dev/null \
        || { log "  WARNING: calibration alloc failed; defaulting MAX_IO_BW to 500 MB/s"
             MAX_IO_BW_MBS_PER_WORKER=500; return; }

    local calib_out
    calib_out=$(python3 "$IO_WORKER_SCRIPT" "$calib_file" 0 "$IO_BLOCK_SIZE" 2 2>&1) || true
    rm -f "$calib_file" 2>/dev/null || true

    MAX_IO_BW_MBS_PER_WORKER=$(python3 -c "
import re, sys
m = re.search(r'actual=([0-9.]+)\s+MB/s', '''$calib_out''')
if m:
    print(max(1, int(float(m.group(1)))))
else:
    sys.exit(1)
" 2>/dev/null) || {
        MAX_IO_BW_MBS_PER_WORKER=500
        log "  WARNING: calibration parse failed; defaulting MAX_IO_BW to $MAX_IO_BW_MBS_PER_WORKER MB/s"
        log "    Set IBENCH_IO_SATURATE_MBS=<N> to override."
        return
    }

    log "  IO saturate BW:  $MAX_IO_BW_MBS_PER_WORKER MB/s per worker"
    log "  IO duty mapping: 10% → $(python3 -c "print(round($MAX_IO_BW_MBS_PER_WORKER * 10/100, 1))") MB/s per worker per unit"
}

# start_io_workers <n_workers> <rate_mbs_per_worker> <block_size> <duration_s>
# Workers open pre-allocated scratch files (ibench_io_probe_0.dat, …).
start_io_workers() {
    local n_workers="$1" rate_mbs="$2" block_sz="$3" duration="$4"
    IO_WORKER_PID=""
    if (( n_workers <= 0 )); then return; fi
    log "    IO  workers: $n_workers × ${rate_mbs} MB/s, ${block_sz}B blocks, ${duration}s"
    local io_scratch="$IO_SCRATCH_DIR"
    local io_script="$IO_WORKER_SCRIPT"
    (
        pids=()
        for (( i=0; i<n_workers; i++ )); do
            python3 "$io_script" \
                "${io_scratch}/ibench_io_probe_${i}.dat" \
                "$rate_mbs" "$block_sz" "$duration" &
            pids+=($!)
        done
        for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    ) &
    IO_WORKER_PID=$!
}

stop_io_workers() {
    set +e
    if [[ -n "${IO_WORKER_PID:-}" ]]; then
        kill "$IO_WORKER_PID" 2>/dev/null
        wait "$IO_WORKER_PID" 2>/dev/null
        IO_WORKER_PID=""
    fi
    set -e
}

# ── DB helpers (mirrors saturation_sweep.sh exactly) ─────────────────────────

drop_page_cache() {
    [[ "$DROP_CACHES" != "true" ]] && return
    log "  Dropping OS page cache ..."
    sudo sysctl -w vm.dirty_expire_centisecs=0 >/dev/null 2>&1 || true
    if echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1; then
        log "  Page cache dropped."
    else
        log "  WARNING: drop_caches failed — reads may be cache-served."
    fi
}

create_spec() {
    TMPSPEC=$(mktemp "${SAT_TMPDIR}/botdet_spec_XXXXX.spec")
    cp "$WORKLOAD_SPEC" "$TMPSPEC"
    printf '\n' >> "$TMPSPEC"
    [[ -n "$RECORD_COUNT" ]] && {
        printf 'recordcount=%s\n'    "$RECORD_COUNT" >> "$TMPSPEC"
        printf 'operationcount=%s\n' "$RECORD_COUNT" >> "$TMPSPEC"
    }
    for kv in "$@"; do printf '%s\n' "$kv" >> "$TMPSPEC"; done
    echo "$TMPSPEC"
}

load_db() {
    local dbpath="$1" logfile="${2:-$OUTPUT_DIR/load.log}"
    log "  Loading $RECORD_COUNT records → $dbpath ..."
    local spec; spec=$(create_spec)
    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap true -threads 8 \
        -load true -run false -throughput false \
        -runtime 0 -levels 7 -table baseline \
        2>&1 | tee "$logfile"
    rm -f "$spec"; TMPSPEC=""
    log "  Load complete."
}

# ── Xput helpers ──────────────────────────────────────────────────────────────

parse_xput() {
    python3 -c "
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r'throughput mean:\s*([\d.eE+\-]+)', text)
print(m.group(1) if m else 'nan')" "$1"
}

compute_drop_pct() {
    # compute_drop_pct <baseline_xput> <probe_xput>  →  prints drop% or 'nan'
    python3 -c "
import math
b, p = float('${1}'), float('${2}')
if math.isnan(b) or math.isnan(p) or b <= 0:
    print('nan')
else:
    print(f'{(b - p) / b * 100:.2f}')"
}

# Returns exit-code 0 (true) when drop_pct exceeds DROP_THRESHOLD_PCT.
exceeds_threshold() {
    python3 -c "
import math, sys
v = float('${1}')
sys.exit(0 if not math.isnan(v) and v > ${DROP_THRESHOLD_PCT} else 1)"
}

# ── Single probe run ──────────────────────────────────────────────────────────
# run_probe <run_dir> <dbpath> <n_cpu_full> <cpu_partial_pct> <n_io> <io_rate_mbs>
# n_cpu_full       — fully-saturated iBench CPU threads
# cpu_partial_pct  — duty % for 1 optional partial CPU thread (0 = none)

run_probe() {
    local run_dir="$1"  dbpath="$2"
    local n_cpu_full="$3"  cpu_partial_pct="$4"
    local n_io="$5"        io_rate_mbs="$6"
    local sys_csv="$run_dir/system.csv"
    local log_file="$run_dir/ycsb.log"
    local cmp_csv="$run_dir/compaction_metrics.csv"
    local spec; spec=$(create_spec "metrics_output=${cmp_csv}")

    start_monitor "$sys_csv"

    "$BINARY" \
        -db baseline -dbpath "$dbpath" -P "$spec" \
        -bootstrap false -threads "$KNEE_THREADS" \
        -load false -run false -throughput true \
        -runtime "$PROBE_RUNTIME_SECS" \
        -skip   "$PROBE_WARMUP_S" \
        -levels 7 -table baseline \
        -dbstatistics true \
        2>&1 | tee "$log_file" &
    local ycsb_pid=$!

    start_cpu_workers "$n_cpu_full" "$cpu_partial_pct" "$PROBE_RUNTIME_SECS"
    start_io_workers  "$n_io"  "$io_rate_mbs" "$IO_BLOCK_SIZE" "$PROBE_RUNTIME_SECS"

    wait "$ycsb_pid" 2>/dev/null || true
    rm -f "$spec"; TMPSPEC=""

    stop_cpu_workers
    stop_io_workers
    stop_monitor

    log "    → log: $log_file  sys: $sys_csv"
}

# ── Pre-allocate IO scratch files ─────────────────────────────────────────────
# IO scratch file is reused across probes; cleaned up on EXIT.

preallocate_io_scratch() {
    log "Pre-allocating 1 IO scratch file × ${IO_DATA_SIZE_MB} MiB ..."
    alloc_io_file "${IO_SCRATCH_DIR}/ibench_io_probe_0.dat" "$IO_DATA_SIZE_MB"
    # Drop caches so pre-allocation doesn't pollute the first probe's page cache.
    drop_page_cache
    log "  IO scratch pre-allocation complete."
}

# ── Main identification sweep ─────────────────────────────────────────────────

run_identification() {
    local dbpath="$OUTPUT_DIR/rocksdb"
    local result_csv="$OUTPUT_DIR/results.csv"

    # CSV header
    printf 'probe_label,intensity,n_cpu_full,cpu_partial_pct,' > "$result_csv"
    printf 'io_rate_mbs,xput_mean,xput_drop_pct,triggered,verdict\n' >> "$result_csv"

    local baseline_xput="$KNEE_XPUT"
    sep
    log "Using KNEE_XPUT=$baseline_xput ops/s as baseline (from saturation sweep)."

    # ── Intensity sweep ───────────────────────────────────────────────────────
    local found_bottleneck="false"
    local cpu_bound="false"
    local io_bound="false"
    local intensity=1

    while [[ "$found_bottleneck" == "false" ]]; do

        # CPU: intensity × CPU_UNIT_PCT% of all NCORES cores.
        local n_cpu_full cpu_partial_pct
        read -r n_cpu_full cpu_partial_pct < <(python3 -c "
import math
target = $intensity * $CPU_UNIT_PCT / 100.0 * $NCORES
n = int(math.floor(target))
p = max(0, min(99, int(round((target - n) * 100))))
print(n, p)
")
        local cpu_target_pct=$(( intensity * CPU_UNIT_PCT ))

        # IO: intensity × IO_UNIT_PCT% of max device BW (1 worker, rate-limited).
        local io_rate_mbs
        io_rate_mbs=$(python3 -c "
print(max(1, round($MAX_IO_BW_MBS_PER_WORKER * $intensity * $IO_UNIT_PCT / 100)))
") || io_rate_mbs=1

        sep
        log "Intensity=$intensity"
        log "  CPU: ${cpu_target_pct}% of $NCORES cores → $n_cpu_full full thread(s) + ${cpu_partial_pct}% partial"
        log "  IO:  ${io_rate_mbs} MB/s  (${IO_UNIT_PCT}% × $intensity of ${MAX_IO_BW_MBS_PER_WORKER} MB/s max)"

        # ── CPU probe ─────────────────────────────────────────────────────────
        log ""
        log "  ▶ CPU probe (Intensity=$intensity)"
        local cpu_dir="$OUTPUT_DIR/cpu_probe_i${intensity}"
        mkdir -p "$cpu_dir"
        rm -rf "$dbpath"
        load_db "$dbpath" "$cpu_dir/load.log"
        drop_page_cache
        run_probe "$cpu_dir" "$dbpath" "$n_cpu_full" "$cpu_partial_pct" 0 0

        local cpu_xput cpu_drop cpu_flag
        cpu_xput=$(parse_xput  "$cpu_dir/ycsb.log" 2>/dev/null) || cpu_xput="nan"
        cpu_drop=$(compute_drop_pct "$baseline_xput" "$cpu_xput") || cpu_drop="nan"
        log "    parsed: xput=$cpu_xput  drop=$cpu_drop%  threshold=${DROP_THRESHOLD_PCT}%"

        if exceeds_threshold "$cpu_drop"; then
            cpu_flag="yes"; found_bottleneck="true"; cpu_bound="true"
            log "    ✓ CPU-BOUND triggered: xput=${cpu_xput}  drop=${cpu_drop}%"
        else
            cpu_flag="no"
            log "    ✗ No CPU bottleneck: xput=${cpu_xput}  drop=${cpu_drop}%"
        fi

        printf 'cpu_probe_i%d,%d,%d,%d,0,%s,%s,%s,%s\n' \
            "$intensity" "$intensity" \
            "$n_cpu_full" "$cpu_partial_pct" \
            "$cpu_xput" "$cpu_drop" "$cpu_flag" \
            "$([ "$cpu_flag" = yes ] && echo cpu_bound || echo -)" \
            >> "$result_csv"

        # ── IO probe ──────────────────────────────────────────────────────────
        log ""
        log "  ▶ IO probe  (Intensity=$intensity)"
        local io_dir="$OUTPUT_DIR/io_probe_i${intensity}"
        mkdir -p "$io_dir"
        rm -rf "$dbpath"
        load_db "$dbpath" "$io_dir/load.log"
        drop_page_cache
        run_probe "$io_dir" "$dbpath" 0 0 1 "$io_rate_mbs"

        local io_xput io_drop io_flag
        io_xput=$(parse_xput  "$io_dir/ycsb.log" 2>/dev/null) || io_xput="nan"
        io_drop=$(compute_drop_pct "$baseline_xput" "$io_xput") || io_drop="nan"
        log "    parsed: xput=$io_xput  drop=$io_drop%  threshold=${DROP_THRESHOLD_PCT}%"

        if exceeds_threshold "$io_drop"; then
            io_flag="yes"; found_bottleneck="true"; io_bound="true"
            log "    ✓ IO-BOUND triggered: xput=${io_xput}  drop=${io_drop}%"
        else
            io_flag="no"
            log "    ✗ No IO bottleneck:  xput=${io_xput}  drop=${io_drop}%"
        fi

        printf 'io_probe_i%d,%d,0,0,%d,%s,%s,%s,%s\n' \
            "$intensity" "$intensity" \
            "$io_rate_mbs" \
            "$io_xput" "$io_drop" "$io_flag" \
            "$([ "$io_flag" = yes ] && echo io_bound || echo -)" \
            >> "$result_csv"

        # ── Escalation ────────────────────────────────────────────────────────
        if [[ "$found_bottleneck" == "false" ]]; then
            if (( intensity >= MAX_INTENSITY )); then
                log "  ✗ MAX_INTENSITY ($MAX_INTENSITY) reached with no bottleneck — inconclusive."
                break
            fi
            (( intensity += INTENSITY_STEP ))
            (( intensity > MAX_INTENSITY )) && intensity=$MAX_INTENSITY
            log "  → Escalating to Intensity=$intensity"
        fi

    done

    # ── Final verdict ─────────────────────────────────────────────────────────
    local verdict
    if   [[ "$cpu_bound" == "true" && "$io_bound" == "true" ]]; then
        verdict="cpu_and_io_bound"
    elif [[ "$cpu_bound" == "true" ]]; then
        verdict="cpu_bound"
    elif [[ "$io_bound" == "true" ]]; then
        verdict="io_bound"
    else
        verdict="inconclusive"
    fi

    sep
    case "$verdict" in
        cpu_bound)
            log "✓ VERDICT: CPU-BOUND"
            log "  CPU interference caused a >${DROP_THRESHOLD_PCT}% throughput drop."
            log "  IO interference did NOT cause a significant drop."
            ;;
        io_bound)
            log "✓ VERDICT: IO-BOUND"
            log "  IO interference caused a >${DROP_THRESHOLD_PCT}% throughput drop."
            log "  CPU interference did NOT cause a significant drop."
            ;;
        cpu_and_io_bound)
            log "✓ VERDICT: CPU-AND-IO-BOUND"
            log "  Both CPU and IO interference triggered a >${DROP_THRESHOLD_PCT}% drop"
            log "  at Intensity=$intensity."
            log "  The workload competes for both resources simultaneously."
            ;;
        inconclusive)
            log "⚠ VERDICT: INCONCLUSIVE"
            log "  Neither probe triggered a >${DROP_THRESHOLD_PCT}% drop"
            log "  up to Intensity=$MAX_INTENSITY."
            log "  Consider: lowering DROP_THRESHOLD_PCT, raising MAX_INTENSITY,"
            log "  or raising CPU_UNIT_PCT / IO_UNIT_PCT."
            ;;
    esac

    log ""
    log "Results → $result_csv"
    echo ""

    # Print summary table.
    python3 - "$result_csv" << 'PYEND'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
cols   = ['probe_label','intensity','n_cpu_full','cpu_partial_pct',
          'io_rate_mbs','xput_mean','xput_drop_pct','triggered','verdict']
widths = [20, 9, 11, 15, 12, 14, 13, 10, 18]
hdr = '  '.join(c.ljust(w) for c, w in zip(cols, widths))
print(hdr)
print('-' * len(hdr))
for r in rows:
    print('  '.join(str(r.get(c,'')).ljust(w) for c, w in zip(cols, widths)))
PYEND
}

# ── Main ──────────────────────────────────────────────────────────────────────

main() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --knee-threads=*)       KNEE_THREADS="${1#*=}" ;;
            --knee-threads)         KNEE_THREADS="$2"; shift ;;
            --knee-xput=*)          KNEE_XPUT="${1#*=}" ;;
            --knee-xput)            KNEE_XPUT="$2"; shift ;;
            --probe-runtime=*)      PROBE_RUNTIME_SECS="${1#*=}" ;;
            --probe-runtime)        PROBE_RUNTIME_SECS="$2"; shift ;;
            --probe-warmup=*)       PROBE_WARMUP_S="${1#*=}" ;;
            --probe-warmup)         PROBE_WARMUP_S="$2"; shift ;;
            --drop-threshold=*)     DROP_THRESHOLD_PCT="${1#*=}" ;;
            --drop-threshold)       DROP_THRESHOLD_PCT="$2"; shift ;;
            --max-intensity=*)      MAX_INTENSITY="${1#*=}" ;;
            --max-intensity)        MAX_INTENSITY="$2"; shift ;;
            --intensity-step=*)     INTENSITY_STEP="${1#*=}" ;;
            --intensity-step)       INTENSITY_STEP="$2"; shift ;;
            --cpu-unit-pct=*)       CPU_UNIT_PCT="${1#*=}" ;;
            --cpu-unit-pct)         CPU_UNIT_PCT="$2"; shift ;;
            --worker-rounds=*)      WORKER_ROUNDS_MULTIPLIER="${1#*=}" ;;
            --worker-rounds)        WORKER_ROUNDS_MULTIPLIER="$2"; shift ;;
            --saturate-rate=*)      IBENCH_SATURATE_RATE="${1#*=}" ;;
            --saturate-rate)        IBENCH_SATURATE_RATE="$2"; shift ;;
            --io-unit-pct=*)         IO_UNIT_PCT="${1#*=}" ;;
            --io-unit-pct)           IO_UNIT_PCT="$2"; shift ;;
            --io-saturate-mbs=*)     IBENCH_IO_SATURATE_MBS="${1#*=}" ;;
            --io-saturate-mbs)       IBENCH_IO_SATURATE_MBS="$2"; shift ;;
            --io-block-size=*)       IO_BLOCK_SIZE="${1#*=}" ;;
            --io-block-size)         IO_BLOCK_SIZE="$2"; shift ;;
            --io-data-size-mb=*)     IO_DATA_SIZE_MB="${1#*=}" ;;
            --io-data-size-mb)       IO_DATA_SIZE_MB="$2"; shift ;;
            --io-scratch-dir=*)     IO_SCRATCH_DIR="${1#*=}" ;;
            --io-scratch-dir)       IO_SCRATCH_DIR="$2"; shift ;;
            *) die "Unknown argument: $1" ;;
        esac
        shift
    done

    check_prereqs

    # Worst-case DB loads: 2 probes × ceil((MAX_INTENSITY) / INTENSITY_STEP) levels.
    local inner_steps; inner_steps=$(python3 -c "
import math; print(math.ceil($MAX_INTENSITY / $INTENSITY_STEP))")
    local total_probes=$(( 2 * inner_steps ))
    local load_gib=$(( RECORD_COUNT * 2064 / 1024 / 1024 / 1024 ))

    log "identify_bottleneck.sh"
    log "  Binary:           $BINARY"
    log "  Output dir:       $OUTPUT_DIR"
    log "  Workload:         $WORKLOAD_LABEL"
    log "  Knee threads:     $KNEE_THREADS"
    log "  Knee xput:        $KNEE_XPUT ops/s  (from saturation sweep)"
    log "  Probe runtime:    ${PROBE_RUNTIME_SECS}s  (warmup: ${PROBE_WARMUP_S}s)"
    log "  Drop threshold:   ${DROP_THRESHOLD_PCT}%"
    log "  Sweep:            Intensity 1..${MAX_INTENSITY}  step=${INTENSITY_STEP}"
    log "  CPU unit:         ${CPU_UNIT_PCT}% of $NCORES cores per intensity unit"
    log "  IO unit:          ${IO_UNIT_PCT}% of ${MAX_IO_BW_MBS_PER_WORKER} MB/s per intensity unit"
    log "  IO scratch dir:   $IO_SCRATCH_DIR"
    echo ""
    log "  DB size:          ~${load_gib} GiB  (${RECORD_COUNT} records)"
    log "  Worst-case probes: 2 × ${inner_steps} = ${total_probes} DB loads"
    echo ""

    mkdir -p "$OUTPUT_DIR"
    get_ncores
    setup_monitor_script
    compile_ibench_cpu
    calibrate_cpu_rate
    setup_io_worker_script
    preallocate_io_scratch
    calibrate_io_rate

    run_identification

    sep
    log "Done. → $OUTPUT_DIR"
}

main "$@"
