rm ../../test_result/test-mycelium/*
rm ../../test_result/test-rocksdb/*
rm ../../test_result/test-precracking/*
rm ../../test_result/test-writetwice/*
./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true
./src/test/ycsb/ycsb_test -db rocksdb -dbpath /holly/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false
./src/test/ycsb/ycsb_test -db rocksdb_column_strawman -dbpath /holly/test_result/test-precracking -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false
./src/test/ycsb/ycsb_test -db writetwice -dbpath /holly/test_result/test-writetwice -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -table writetwice


for i in {1..2}
do
    ./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true

    ./src/test/ycsb/ycsb_test -db rocksdb -dbpath /holly/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

    ./src/test/ycsb/ycsb_test -db rocksdb_column_strawman -dbpath /holly/test_result/test-precracking -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

    ./src/test/ycsb/ycsb_test -db writetwice -dbpath /holly/test_result/test-writetwice -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -table writetwice

done
