# Yahoo! Cloud System Benchmark
# Workload D: Read latest workload
#   Application example: user status updates; people want to read the latest
#                        
#   Read/update/insert ratio: 95/0/5
#   Default data size: 1 KB records (16 fields, 64 bytes each, plus key)
#   Request distribution: latest

# The insert order for this is hashed, not ordered. The "latest" items may be 
# scattered around the keyspace if they are keyed by userid.timestamp. A workload
# which orders items purely by time, and demands the latest, is very different than 
# workload here (which we believe is more typical of how people build systems.)
keylength=16
fieldcount=15
fieldlength=16

recordcount=25000000
operationcount=20000
workload=com.yahoo.ycsb.workloads.CoreWorkload

readallfields=false
requestdistribution=latest

readproportion=0
updateproportion=0
scanproportion=1.0
insertproportion=0