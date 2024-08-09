# htap
HTAP is a hybrid transactional and analytical workload processing data platform built on RocksDB. In addition to its excellent write throughput strengths similar to RocksDB, HTAP features its capability of transforming in-place  incoming raw data into a format optimized for efficient future reads of workloads. Examples of such transformations include splitting data into several column groups, converting data formats (e.g., from JSON to flat buffers), and creating indexes to enhance query performance.

Directory lib/rocksdb contains the source code of the Mycelium storage engine built using RocksDB 5.14.
Directory src/test contains benchmark files used to generate and test workloads.

### Setup
#### - Get the source code  
git clone https://github.com/hcasalet/htap.git  
cd htap  
git submodule update --init --recursive  

#### - Prepare the dependencies  
% sudo apt-get update
% sudo apt-get install -y cmake libgflags-dev protobuf-compiler ninja-build libsnappy-dev zlib1g-dev libbz2-dev flatbuffers-compiler flatbuffers-compiler-dev libflatbuffers-dev libzstd-dev liblz4-dev libre2-dev

% wget https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-focal.deb
% sudo apt install ./apache-arrow-apt-source-latest-focal.deb
% sudo apt-get update
% sudo apt-get install -y libarrow-dev libparquet-dev

#### - Build code  
% mkdir build; cd build
% cmake -S .. -B . -G Ninja / cmake -S .. -B .
% ninja/make

#### - Debug code  
cmake -DCMAKE_BUILD_TYPE=Debug -S .. -B . -G Ninja  
gdb src/test/ycsb/ycsb_test  
(gdb) b <functionname>

#### - Run YCSB tests example command:  
./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -transform_type 1 -table mycelium

./src/test/ycsb/ycsb_test -db flatbuffers -dbpath /holly/test_result/test-fb -P "../src/test/ycsb/workloads/test_workloadc.spec" -bootstrap false -threads 1 -load false -run true -throughput false -transform false -transform_type 3 -table flatbuffers
