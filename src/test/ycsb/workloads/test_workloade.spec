# Yahoo! Cloud System Benchmark
# Workload E: Range queries only workload
#   Application example: threaded conversations, where each scan is for the posts in a given thread (assumed to be clustered by thread id)
#                        
#   Scan/insert ratio: 95/5
#   Default data size: 1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: zipfian

# The insert order is hashed, not ordered. Although the scans are ordered, it does not necessarily
# follow that the data is inserted in order. For example, posts for thread 342 may not be inserted contiguously, but
# instead interspersed with posts from lots of other threads. The way the YCSB client works is that it will pick a start
# key, and then request a number of records; this works fine even for hashed insertion.
keylength=32
fieldcount=16
fieldlength=64

recordcount=100000
operationcount=100000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=true
indexaccess=true

readproportion=0
updateproportion=0
scanproportion=1.0
insertproportion=0

requestdistribution=zipfian
maxscanlength=100

scanlengthdistribution=uniform


