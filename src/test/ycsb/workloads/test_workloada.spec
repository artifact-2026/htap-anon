# Yahoo! Cloud System Benchmark
# Workload A: Update heavy workload
#   Application example: Session store recording recent actions
#                        
#   Read/update ratio: 0/100
#   Default data size: 1 KB records (10 fields, 100 bytes each, plus key)
#   Request distribution: zipfian
keylength=16
fieldcount=16
fieldlength=128

recordcount=5000000
operationcount=5000000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false

readproportion=0
updateproportion=0
scanproportion=0
insertproportion=1.0

requestdistribution=zipfian

