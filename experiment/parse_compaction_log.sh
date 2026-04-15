# get elapsed wall time
grep -E 'EVENT_LOG_v1.*"event": "compaction_finished"' LOG \
| sed -E -n 's/.*"compaction_time_micros": ([0-9]+).*/\1/p' \
| awk '{s+=$1} END{printf "Total compaction wall time: %.3fs (%.2f min)\n", s/1e6, s/1e6/60}'

# get CPU time
grep -E 'EVENT_LOG_v1.*"event": "compaction_finished"' LOG \
| sed -E -n 's/.*"compaction_time_cpu_micros": ([0-9]+).*/\1/p' \
| awk '{s+=$1} END{printf "Total compaction CPU time: %.3fs\n", s/1e6}'

15:37:41.485165
15:55:36.349972