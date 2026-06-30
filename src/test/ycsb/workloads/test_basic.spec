# Yahoo! Cloud System Benchmark
# Workload A: Update heavy workload
#   Application example: Session store recording recent actions
#                        
#   Read/update ratio: 50/50
#   Default data size: 1 KB records (10 fields, 100 bytes each, plus key)
#   Request distribution: zipfian
keylength=128
fieldcount=64
fieldlength=128

recordcount=1000
operationcount=1000

workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false

readproportion=1.0
updateproportion=0.0
scanproportion=0
insertproportion=0
