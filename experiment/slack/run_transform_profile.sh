#!/usr/bin/env bash
# =============================================================================
# run_transform_profile.sh — Dispatcher
#
# Thin dispatcher that forwards to the appropriate per-benchmark harness.
# The first argument must be --bench <type>.
#
# Individual harnesses (preferred for direct invocation):
#   run_split_transform_profile.sh    — split_transform_bench
#   run_convert_transform_profile.sh  — convert_transform_bench
#   run_augment_transform_profile.sh  — augment_transform_bench
#
# Usage:
#   bash run_transform_profile.sh --bench <split|convert|augment> [args...]
#
# Equivalent direct invocations:
#   bash run_split_transform_profile.sh   <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]
#   bash run_convert_transform_profile.sh <duration_s> <n_workers> <num_fields> <field_length> <mode>
#   bash run_augment_transform_profile.sh <duration_s> <n_workers> <num_fields> <field_length> <index_mode> <field_idx> [...]
#
# Examples:
#   bash run_transform_profile.sh --bench split   10 4 16 128 4 json
#   bash run_transform_profile.sh --bench convert 10 4 16 128 csv2json
#   bash run_transform_profile.sh --bench augment 10 4 16 128 hash 0 3 7
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$#" -lt 2 ] || [ "$1" != "--bench" ]; then
    echo "Usage: $0 --bench <split|convert|augment> [benchmark-specific args...]"
    echo ""
    echo "  split:   $0 --bench split   <duration_s> <n_workers> <num_fields> <field_length> <x_ways> [csv|json]"
    echo "  convert: $0 --bench convert <duration_s> <n_workers> <num_fields> <field_length> <mode>"
    echo "  augment: $0 --bench augment <duration_s> <n_workers> <num_fields> <field_length> <index_mode> <field_idx> [...]"
    exit 1
fi

BENCH_TYPE="$2"
shift 2

case "$BENCH_TYPE" in
    split)
        exec bash "$SCRIPT_DIR/run_split_transform_profile.sh" "$@"
        ;;
    convert)
        exec bash "$SCRIPT_DIR/run_convert_transform_profile.sh" "$@"
        ;;
    augment)
        exec bash "$SCRIPT_DIR/run_augment_transform_profile.sh" "$@"
        ;;
    *)
        echo "ERROR: unknown bench type '$BENCH_TYPE'. Use: split | convert | augment"
        exit 1
        ;;
esac
