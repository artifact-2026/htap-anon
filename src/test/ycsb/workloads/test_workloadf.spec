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
indexaccess=true

readproportion=0
updateproportion=0
scanproportion=1.0
insertproportion=0
readmodifywriteproportion=0

requestdistribution=leastrecent
