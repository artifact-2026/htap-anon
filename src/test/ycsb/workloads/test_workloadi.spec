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

recordcount=20000000
operationcount=200000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false

readproportion=0.4
updateproportion=0.15
scanproportion=0.1
insertproportion=0.3
readmodifywriteproportion=0.05

requestdistribution=leastrecent