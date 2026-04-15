#!/bin/bash

VMSTAT_FILE="${1:-vmstat_log.txt}"
IOSTAT_FILE="${2:-iostat_log.txt}"

# === Analyze vmstat ===
read avg_user avg_sys avg_idle avg_io_wait <<< $(awk '
BEGIN { user=0; sys=0; idle=0; iowait=0; count=0 }
/^procs/ { next }
NF==17 {
    user   += $13
    sys    += $14
    idle   += $15
    iowait += $16
    count++
}
END {
    if (count > 0) {
        printf "%.4f %.5f %.4f %.5f\n", user/count, sys/count, idle/count, iowait/count
    }
}' "$VMSTAT_FILE")

# === Analyze iostat ===
avg_disk_util=$(awk '
BEGIN { util=0; count=0 }
/^Device/ { next }
/^Linux/ { next }
NF > 0 && $1 ~ /^nvme/ {
    util += $(NF)   # %util is always the last field
    count++
}
END {
    if (count > 0) {
        printf "%.4f\n", util/count
    }
}' "$IOSTAT_FILE")

# === Determine verdict ===
verdict="Not strongly bound by CPU or I/O"
cpu_sum=$(echo "$avg_user + $avg_sys" | bc -l)

if (( $(echo "$avg_io_wait > 30" | bc -l) )) && (( $(echo "$avg_disk_util > 70" | bc -l) )); then
    verdict="Likely IO bound"
elif (( $(echo "$cpu_sum > 70" | bc -l) )); then
    verdict="Likely CPU bound"
fi

# === Output results ===
echo "=== Analyzing $VMSTAT_FILE ==="
printf "Avg user CPU:   %.4f %%\n" "$avg_user"
printf "Avg system CPU: %.5f %%\n" "$avg_sys"
printf "Avg idle CPU:   %.4f %%\n" "$avg_idle"
printf "Avg IO wait:    %.5f %%\n\n" "$avg_io_wait"

echo "=== Analyzing $IOSTAT_FILE ==="
printf "Avg disk %%util: %.4f %%\n\n" "$avg_disk_util"

echo "Verdict: $verdict"
