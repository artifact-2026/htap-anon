ycsb_cmd=( ./src/test/ycsb/ycsb_test
  -db mynoop
  -dbpath "/holly/test_results/mynoop"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table mynoop
)
