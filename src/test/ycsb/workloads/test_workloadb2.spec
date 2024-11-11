# Yahoo! Cloud System Benchmark
# Workload B: Point query, reading 1 column
#   Application example: photo tagging; add a tag is an update, but most operations are to read tags
#                        
#   Read/update ratio: 95/5
#   Default data size: 1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian
keylength=16
fieldcount=15
fieldlength=16

recordcount=25000000
operationcount=20000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true
requestdistribution=zipfian

readproportion=1.0
updateproportion=0
scanproportion=0
insertproportion=0