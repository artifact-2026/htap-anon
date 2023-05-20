# Yahoo! Cloud System Benchmark
# Workload A: Write only workload
#   Application example: Traffic monitoring sensor recording data
#                        
#   Insert ratio: 100
#   Default data size: ~1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian

keylength=32
fieldcount=16
fieldlength=64
recordcount=100000
operationcount=100000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readproportion=0
updateproportion=0
scanproportion=0
insertproportion=1.0
