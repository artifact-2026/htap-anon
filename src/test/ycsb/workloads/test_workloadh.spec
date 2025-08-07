# Yahoo! Cloud System Benchmark
# Workload F: read workload
#   Application example: analytical workload, or business intelligence workload
#                        
#   read/write ratio: 100/0
#   Default data size: 50 KB records (100 fields, 512 bytes each, plus key)
#   Request distribution: least recent
keylength=16
fieldcount=100
fieldlength=512

recordcount=20000000
operationcount=2000000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true

readproportion=1.0
updateproportion=0
scanproportion=0
insertproportion=0
readmodifywriteproportion=0

requestdistribution=leastrecent