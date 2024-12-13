# Yahoo! Cloud System Benchmark
# Workload A: Point Query, reading entire row
#   Application example: Traffic monitoring sensor recording data
#                        
#   Insert ratio: 100
#   Default data size: ~1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian

keylength=16
fieldcount=32
fieldlength=16
recordcount=10000000
operationcount=200000
dataformat=json
columndatatype=numeric
workload=com.yahoo.ycsb.workloads.CoreWorkload

readproportion=0
updateproportion=0
scanproportion=0
insertproportion=1.0

readallfields=true
requestdistribution=zipfian