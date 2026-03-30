#!/usr/bin/env bash
# =============================================================================
# calibrate_transform_rate.sh — derive TRANSFORMS_PER_WORKER for Phase 2
#
# Reads the saturation_summary.csv produced by saturation_sweep.sh, identifies
# the knee row, and estimates the compaction output rate per thread from the
# measured disk bandwidth.  Prints a step-by-step derivation and writes a
# phase2_config.env file ready to be sourced into cpu_slack_sweep.sh.
#
# Usage:
#   bash ../utility/calibrate_transform_rate.sh <phase1_output_dir> \
#        [--compaction-threads N] [--workload-spec <spec>]
#
# The --compaction-threads value must match the max_background_compactions
# setting used in the Phase 1 RocksDB instance.  If you did not set it
# explicitly, RocksDB defaults to max_background_jobs/2.  The ycsb_test
# binary as configured in this project passes -levels 6 but does not
# override max_background_jobs, so the default of 2 applies (= 1 compaction
# thread).  Verify with:  grep -i background your_rocksdb_options_file
#
# =============================================================================

set -euo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────────

PHASE1_DIR=""
COMPACTION_THREADS=1       # default: RocksDB default (max_background_jobs/2 = 1)
FIELD_COUNT=16             # must match saturation_sweep.sh defaults
FIELD_LENGTH=256
KEY_LENGTH=16
WORKLOAD_SPEC=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --compaction-threads) COMPACTION_THREADS="$2"; shift 2 ;;
        --key-length)         KEY_LENGTH="$2";         shift 2 ;;
        --workload-spec)      WORKLOAD_SPEC="$2";      shift 2 ;;
        -*)  echo "Unknown option: $1" >&2; exit 1 ;;
        *)   PHASE1_DIR="$1"; shift ;;
    esac
done

[[ -n "$PHASE1_DIR" ]] || {
    echo "Usage: $0 <phase1_output_dir> [--compaction-threads N] [--workload-spec <spec>]"
    echo ""
    echo "  phase1_output_dir   directory written by saturation_sweep.sh"
    echo "                      (contains summary.csv)"
    echo "  --compaction-threads N   RocksDB max_background_compactions (default: 1)"
    exit 1
}

SUMMARY_CSV="$PHASE1_DIR/summary.csv"
[[ -f "$SUMMARY_CSV" ]] || {
    echo "ERROR: summary CSV not found: $SUMMARY_CSV"
    echo "  Run saturation_sweep.sh first and pass its output directory."
    exit 1
}

ENV_OUT="$PHASE1_DIR/phase2_config.env"

# ── Embedded Python analysis ──────────────────────────────────────────────────

python3 - "$SUMMARY_CSV" "$ENV_OUT" \
          "$COMPACTION_THREADS" "$WORKLOAD_SPEC" << 'PYEOF'
import sys, math
import pandas as pd
import numpy as np

summary_csv       = sys.argv[1]
env_out           = sys.argv[2]
compaction_threads= int(sys.argv[3])
workload_spec     = sys.argv[4]

def get_spec_value(path: str, key: str) -> str | None:
    value = None
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue

            lhs, rhs = line.split("=", 1)
            if lhs.strip() == key:
                value = rhs.strip()   # keeps the last matching entry
    return value

field_count = int(get_spec_value(workload_spec, "fieldcount"))
field_length = int(get_spec_value(workload_spec, "fieldlength"))
key_length   = int(get_spec_value(workload_spec, "keylength"))
record_size = field_count * field_length + key_length   # bytes per record

# ── Load summary and find knee ────────────────────────────────────────────────
df = pd.read_csv(summary_csv)
df = df.sort_values('threads').reset_index(drop=True)

valid = df.dropna(subset=['xput_mean'])
if valid.empty:
    print("ERROR: no valid throughput data in summary CSV.")
    sys.exit(1)

peak_xput = valid['xput_mean'].max()

# Knee = first row where the marginal throughput gain over the previous row
# drops below 10%.  Walk the sorted rows pairwise; the knee is the later of
# the two rows that form the first sub-10%-gain step.
# Fall back to the peak row if every step exceeds 10% (unlikely but safe).
xput_vals = valid['xput_mean'].values
knee_idx  = len(valid) - 1          # default: last (peak) row
for i in range(1, len(xput_vals)):
    prev, curr = xput_vals[i - 1], xput_vals[i]
    gain = (curr - prev) / prev if prev > 0 else 0.0
    if gain < 0.075:
        knee_idx = i
        break
knee_row = valid.iloc[knee_idx-1]

knee_threads     = int(knee_row['threads'])
knee_xput        = float(knee_row['xput_mean'])
disk_write_mbs   = float(knee_row['disk_write_mb/s'])
disk_read_mbs    = float(knee_row['disk_read_mb/s'])
cpu_active_pct   = float(knee_row['cpu_active_mean'])

# ── Derivation ────────────────────────────────────────────────────────────────
#
# PRIMARY signal: disk_read_mbs
#   In RocksDB, disk reads are almost exclusively compaction input: SST files
#   being merged from level N into level N+1.  WAL and memtable flushes are
#   write-only; user reads come from the block cache and are typically a tiny
#   fraction of total I/O in a write-heavy workload.  So:
#
#     compaction_input_rate  ≈  disk_read_mbs
#     compaction_output_rate ≤  disk_read_mbs   (output ≤ input; WA ≥ 1)
#
#   Using disk_read as an upper bound on compaction output is conservative:
#   it will never undercount the actual compaction output rate, so it's safe
#   to use as the transform worker rate target.
#
# SECONDARY cross-check: disk_write - user_write
#   disk_write = WAL writes + L0 flush + compaction output
#   WAL  ≈ user_write_rate  (every Put appends to WAL)
#   Flush is pipelined and typically < 1× user data at steady state; treat
#   it as ≈ user_write_rate (conservative, gives a lower bound).
#   → compaction_output ≈ disk_write - 2 × user_write_rate  (lower bound)
#
# The recommendation is disk_read-based (upper bound on output); both
# estimates are printed for cross-validation.

user_write_mbs  = knee_xput * record_size / 1e6   # MB/s of user payload

# PRIMARY: disk_read is compaction input; output ≤ input
compaction_from_read_mbs = disk_read_mbs   # upper bound on compaction output

# SECONDARY cross-check via write side (lower bound)
compaction_write_lower_mbs = max(0.0, disk_write_mbs - 2.0 * user_write_mbs)

# Write amplification estimate (informational)
wa_approx = disk_write_mbs / user_write_mbs if user_write_mbs > 0 else float('nan')

# Use disk_read as the recommended compaction rate (per-thread)
comp_total_per_sec = compaction_from_read_mbs * 1e6 / record_size
comp_per_thread    = comp_total_per_sec / compaction_threads

# ── Round to a clean value ────────────────────────────────────────────────────
# Round to nearest 100, with a floor of 100.

def round_to(x, base):
    return max(base, int(base * round(x / base)))

recommended_rate = round_to(comp_per_thread, 100)

# ── Warn if disk_read is suspiciously low ─────────────────────────────────────
read_low_warning = disk_read_mbs < 0.5 * user_write_mbs

# ── Suggest a sweep range ─────────────────────────────────────────────────────
# Cover 0 workers (baseline) up to ~4–5× the expected slack budget,
# with finer granularity near the expected boundary.

max_workers = max(16, int(math.ceil(4.0 * (100.0 / max(1, cpu_active_pct - 50)))))
max_workers = min(max_workers, 32)   # cap for sanity

# ── Print derivation ──────────────────────────────────────────────────────────

SEP = "─" * 62
print(SEP)
print("  Compaction rate calibration")
print(SEP)
print(f"  Phase 1 summary:  {summary_csv}")
print()
print(f"  Knee operating point:")
print(f"    Write threads :  {knee_threads}")
print(f"    Throughput    :  {knee_xput:,.0f} ops/s")
print(f"    CPU active    :  {cpu_active_pct:.1f}%  →  {100-cpu_active_pct:.1f}% idle slack")
print(f"    Disk write    :  {disk_write_mbs:.1f} MB/s")
print(f"    Disk read     :  {disk_read_mbs:.1f} MB/s")
print()
print(f"  Record layout:")
print(f"    {field_count} fields × {field_length} B + {key_length} B key  =  {record_size} B/record")
print()
print(f"  Derivation (primary: disk reads = compaction input):")
print(f"    User write rate          {knee_xput:>8,.0f} ops/s × {record_size} B")
print(f"                          =  {user_write_mbs:>7.1f} MB/s")
print(f"    Write amplification (≈)  {wa_approx:>7.1f}×  (disk_write / user_write)")
print(f"")
print(f"    Disk reads (≈ compaction input)  {disk_read_mbs:>6.1f} MB/s  ← primary signal")
print(f"    Compaction output rate   ≤  {disk_read_mbs:>5.1f} MB/s  (upper bound; WA ≥ 1)")
print(f"")
print(f"    Cross-check via write side:")
print(f"      disk_write − 2 × user_write  =  {compaction_write_lower_mbs:>5.1f} MB/s  (lower bound)")
print()
print(f"    Compaction records/s (upper bound, total):  {comp_total_per_sec:,.0f}")
print(f"    Compaction threads:                         {compaction_threads}")
print(f"    Per-thread rate (upper bound):              {comp_per_thread:,.0f} records/s/thread")
print()

if read_low_warning:
    print(f"  WARNING: disk_read ({disk_read_mbs:.1f} MB/s) is unexpectedly low relative to")
    print(f"           user_write ({user_write_mbs:.1f} MB/s).  Compaction may not have reached")
    print(f"           steady state — L0 files may be accumulating rather than being")
    print(f"           compacted.  Consider re-running Phase 1 with a longer RUNTIME_SECS")
    print(f"           or larger record count so the DB stabilizes.  The recommended")
    print(f"           TRANSFORMS_PER_WORKER below may be an underestimate.")
    print()
elif wa_approx < 2.0:
    print(f"  NOTE: write amplification ≈ {wa_approx:.1f}× is lower than typical (expected 5–30×).")
    print(f"        This may mean compaction is lagging or the run was too short.")
    print(f"        Consider re-running Phase 1 with a longer RUNTIME_SECS.")
    print()

print(f"  ┌─────────────────────────────────────────────────────────┐")
print(f"  │  Recommended  TRANSFORMS_PER_WORKER = {recommended_rate:<6}           │")
print(f"  │  (conservative lower-bound; each worker ≈ one           │")
print(f"  │  compaction thread's worth of record processing)         │")
print(f"  └─────────────────────────────────────────────────────────┘")
print()
print(f"  Written to: {env_out}")
print(f"  Source before running cpu_slack_sweep.sh:")
print(f"    source {env_out}")
print(SEP)

# ── Write env file ────────────────────────────────────────────────────────────

with open(env_out, 'w') as f:
    f.write("# Auto-generated by calibrate_transform_rate.sh\n")
    f.write(f"# Phase 1 source:  {summary_csv}\n")
    f.write(f"# Record layout:   {field_count} × {field_length} B = "
            f"{field_count*field_length} B/record\n")
    f.write(f"# Compaction threads used for derivation: {compaction_threads}\n")
    f.write("#\n")
    f.write(f"# Knee:\n")
    f.write(f"export KNEE_THREADS={knee_threads}\n")
    f.write(f"export KNEE_XPUT={knee_xput:.0f}\n")
    f.write(f"export WORKLOAD_SPEC={workload_spec}\n")
    f.write("#\n")
    f.write(f"# Transform rate calibration:\n")
    f.write(f"#   user_write_mbs         = {user_write_mbs:.1f} MB/s\n")
    f.write(f"#   write_amplification    = {wa_approx:.1f}x\n")
    f.write(f"#   disk_read_mbs          = {disk_read_mbs:.1f} MB/s  (primary: compaction input)\n")
    f.write(f"#   compaction_output      <= {compaction_from_read_mbs:.1f} MB/s  (upper bound from reads)\n")
    f.write(f"#   write_side_lower_bound = {compaction_write_lower_mbs:.1f} MB/s  (disk_write - 2*user_write)\n")
    f.write(f"#   per_thread_rate        = {comp_per_thread:.0f} records/s/thread (upper bound)\n")
    f.write(f"export TRANSFORMS_PER_WORKER={recommended_rate}\n")

PYEOF
