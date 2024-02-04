for i in {1..10}
do
    rm ../../test_result/test-mycelium/*
    ./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true

    rm ../../test_result/test-rocksdb/*
    ./src/test/ycsb/ycsb_test -db rocksdb -dbpath /holly/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

    rm ../../test_result/test-precracking/*
    ./src/test/ycsb/ycsb_test -db rocksdb_column_strawman -dbpath /holly/test_result/test-precracking -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

    rm ../../test_result/test-writetwice/*
    ./src/test/ycsb/ycsb_test -db writetwice -dbpath /holly/test_result/test-writetwice -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -table writetwice

done
