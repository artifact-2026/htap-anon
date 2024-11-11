# Yahoo! Cloud System Benchmark
# Workload C: Read only
#   Application example: user profile cache, where profiles are constructed elsewhere (e.g., Hadoop)
#                        
#   Read/update ratio: 100/0
#   Default data size: 1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian
keylength=16
fieldcount=15
fieldlength=16

recordcount=25000000
operationcount=20000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false
requestdistribution=uniform

readproportion=1.0
updateproportion=0
scanproportion=0
insertproportion=0