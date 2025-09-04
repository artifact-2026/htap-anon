# Yahoo! Cloud System Benchmark
# Workload A: Point Query, reading entire row
#   Application example: Traffic monitoring sensor recording data
#                        
#   Insert ratio: 100
#   Default data size: ~1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian

keylength=16
fieldcount=4096
fieldlength=4
recordcount=3000000
operationcount=1000000
inputdataformat=json
columndatatype=numeric
workload=com.yahoo.ycsb.workloads.CoreWorkload

readproportion=0.5
updateproportion=0.5
scanproportion=0
insertproportion=0

readallfields=false
requestdistribution=zipfian