# htap
HTAP is a hybrid transactional and analytical workload processing data platform built on RocksDB. In addition to its excellent write throughput strengths similar to RocksDB, HTAP features its capability of transforming in-place  incoming raw data into a format optimized for efficient future reads of workloads. Examples of such transformations include splitting data into several column groups, converting data formats (e.g., from JSON to flat buffers), and creating indexes to enhance query performance.

Directory lib/rocksdb contains the source code of the Mycelium storage engine built using RocksDB 5.14.
Directory src/test contains benchmark files used to generate and test workloads.

### Setup
#### - Set up ssh key
ssh-keygen -t ed25519 -C "your_email@example.com"
cat ~/.ssh/id_ed25519.pub
Create a new ssh/gpg key at Github.com settings
git remote set-url origin git@github.com:<username>/<repo-name>.git

#### - Set the disk
fdisk -l    //to view
fdisk <device_name>  -- enter n (new partition) -- hit enter for default values -- enter w (write the partition table)

#### - Get the source code  
git clone git@github.com:hcasalet/htap.git
cd htap  
git submodule update --init --recursive  

#### - Prepare the dependencies  
% sudo apt-get update
% sudo apt-get install -y cmake libgflags-dev protobuf-compiler ninja-build g++ libboost-dev libboost-system-dev libsnappy-dev zlib1g-dev libbz2-dev flatbuffers-compiler flatbuffers-compiler-dev libflatbuffers-dev libzstd-dev liblz4-dev libre2-dev libjsoncpp-dev nlohmann-json3-dev libavro-dev

% cd /tmp
% git clone https://github.com/apache/avro.git
% cd avro/lang/c++
% mkdir build && cd build
% cmake ..   -DBOOST_ROOT=/usr   -DBoost_INCLUDE_DIR=/usr/include   -DBoost_LIBRARY_DIR=/usr/lib/x86_64-linux-gnu
% make -j$(nproc)
% sudo make install

#### - Build code  
% mkdir build; cd build
% cmake -S .. -B . -G Ninja / cmake -S .. -B .
% ninja/make

#### - Debug code  
% cmake -DCMAKE_BUILD_TYPE=Debug -S .. -B . -G Ninja
% delete build/src/test/ycsb/output/test-<dbtype>
% gdb src/test/ycsb/ycsb_test  
(gdb) b <functionname>
(gdb) run 

##### building with debug, tools, db_bench, and AddressSanitizer
% cmake -DWITH_TOOLS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -S .. -B . -G Ninja
% cmake -DWITH_BENCHMARK_TOOLS=on -DWITH_TOOLS=on -S .. -B . -G Ninja
% cmake -DWITH_TOOLS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -S .. -B . -G Ninja
% echo 'setenv PATH ${PATH}:/holly/htap/build/src/mycelium/tools' >> ~/.tcshrc
% source ~/.tcshrc

##### using tools after building with -DWITH_TOOLS=ON
% ldb manifest_dump --db=.      (at the <path-to-db>)

##### enable core dump
% limit descriptors 65536
% limit coredumpsize unlimited    ## if ulimit -c returns 0 it means it is disabled
% source ~/.tshrc
% gdb ./<myprogram> core.<pid>

##### use valgrind
% valgrind --leak-check=full --track-origins=yes ./your_program

#### - Git
% git remote set-url origin https://hcasalet:<personal_access_token>1@github.com/hcasalet/htap.git

#### - Run YCSB tests example command:  
./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -transform_type 1 -table mycelium

./src/test/ycsb/ycsb_test -db flatbuffers -dbpath /holly/test_result/test-fb -P "../src/test/ycsb/workloads/test_workloadc.spec" -bootstrap false -threads 1 -load false -run true -throughput false -transform false -transform_type 3 -table flatbuffers

### System resource monitoring when testing
sudo apt-get update
sudo apt-get install sysstat dstat procps iotop htop -y

### Run the transformation tests standalone
#### Location: src/test/transformers/scripts

#### build code
% c++ -std=c++17 -O2 -Wall split_from_hex.cc data.pb.cc -o split_from_hex `pkg-config --cflags --libs protobuf` -pthread
#### run sst_dimp
% ./src/mycelium/tools/sst_dump --command=scan --file="/holly/transformation_input/000011.sst" --output_hex | sed -n 's/^value: //p' \
 | ./split_from_hex "/holly/transformation_output/split1.binpb" "/holly/transformation_output/split2.binpb" \
 > "/holly/transformation_logs/split_stdout.log" 2> "/holly/transformation_logs/split_stderr.log"

### c++ -std=c++17 -O2 -Wall split_column_groups.cc data.pb.cc -o split_column_groups `pkg-config --cflags --libs protobuf` -pthread
