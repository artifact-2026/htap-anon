#!/usr/bin/env bash

rm -rf /holly/sat_exp_db

RUNTIME_SECS=120 \
BINARY=./src/test/ycsb/ycsb_test \
DB_BASE_PATH=/holly/sat_exp_db \
DISK_DEVICE=nvme0c0n1 \
bash ../utility/saturation_sweep.sh
