# Yahoo! Cloud System Benchmark
# Workload A: Update heavy workload
#   Application example: Session store recording recent actions
#                        
#   Read/update ratio: 50/50
#   Default data size: 1 KB records (10 fields, 100 bytes each, plus key)
#   Request distribution: zipfian
keylength=16
fieldcount=16
fieldlength=128

recordcount=12000000
operationcount=12000000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false

readproportion=0.3
updateproportion=0.2
scanproportion=0
insertproportion=0.5

requestdistribution=zipfian

