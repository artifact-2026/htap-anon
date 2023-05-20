# Yahoo! Cloud System Benchmark
# Workload F: mixed workload
#   Application example: analytical workload, or business intelligence workload
#                        
#   Scan/write ratio: 100/0
#   Default data size: 1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian
keylength=32
fieldcount=16
fieldlength=64

recordcount=100000
operationcount=100000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false

readproportion=0.2
updateproportion=0
scanproportion=0.3
insertproportion=0.5
readmodifywriteproportion=0

requestdistribution=latest
maxscanlength=100
scanlengthdistribution=uniform
