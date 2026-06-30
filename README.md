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
rapidjson-dev

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

#### - Debug gdb
% cmake -DCMAKE_BUILD_TYPE=Debug -S .. -B . -G Ninja
% delete build/src/test/ycsb/output/test-<dbtype>
% gdb src/test/ycsb/ycsb_test  
(gdb) b <functionname>
(gdb) run 

#### - Debug system
( Kernel log (OOMs say "Out of memory" / "Killed process <pid>"))
% dmesg -T | egrep -i 'killed process|out of memory|oom' | tail -n 50

( If distro uses journald:)
% journalctl -k -n 200 | egrep -i 'killed process|out of memory|oom'

( checking device)
% iostat -dx -N -t -y 1 3

( To find the disk IO bandwidth )
% fio --name=mixedrw \
    --filename=/data/htap/build/fio_tmp \
    --rw=rw --bs=128k --size=8G \
    --direct=1 --numjobs=4 --group_reporting

##### building with debug, tools, db_bench, and AddressSanitizer
% cmake -DWITH_TOOLS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -S .. -B . -G Ninja
% cmake -DWITH_BENCHMARK_TOOLS=on -DWITH_TOOLS=on -S .. -B . -G Ninja
% cmake -S .. -B . -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
% cmake -DWITH_TOOLS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -S .. -B . -G Ninja
% echo 'setenv PATH ${PATH}:/data/htap/build/src/mycelium/tools' >> ~/.tcshrc
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
./src/test/ycsb/ycsb_test -db mycelium -dbpath /data/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -transform_type 1 -table mycelium

./src/test/ycsb/ycsb_test -db flatbuffers -dbpath /data/test_result/test-fb -P "../src/test/ycsb/workloads/test_workloadc.spec" -bootstrap false -threads 1 -load false -run true -throughput false -transform false -transform_type 3 -table flatbuffers

### System resource monitoring when testing
sudo apt-get update
sudo apt-get install sysstat dstat procps iotop htop -y

### Run the transformation tests standalone
#### Location: src/test/transformers/scripts

#### build code
% c++ -std=c++17 -O2 -Wall split_from_hex.cc data.pb.cc -o split_from_hex `pkg-config --cflags --libs protobuf` -pthread
#### run sst_dimp
% ./src/mycelium/tools/sst_dump --command=scan --file="/data/transformation_input/000011.sst" --output_hex | sed -n 's/^value: //p' \
 | ./split_from_hex "/data/transformation_output/split1.binpb" "/data/transformation_output/split2.binpb" \
 > "/data/transformation_logs/split_stdout.log" 2> "/data/transformation_logs/split_stderr.log"

### c++ -std=c++17 -O2 -Wall split_column_groups.cc data.pb.cc -o split_column_groups `pkg-config --cflags --libs protobuf` -pthread

### To install Arrow
1. . /etc/os-release
echo "$PRETTY_NAME ($VERSION_CODENAME)"      // should see "jammy"
2. sudo apt-get update
   sudo apt-get install -y \
      ca-certificates \
      lsb-release \
      wget \
      curl \
      gnupg \
      dirmngr \
      pkg-config

  mkdir -p ~/.gnupg
  chmod 700 ~/.gnupg

3. sudo mkdir -p /etc/apt/sources.list.d
   set CODENAME=`lsb_release -cs`
   echo "deb [signed-by=/usr/share/keyrings/apache-arrow-archive-keyring.gpg] \
     https://apache.jfrog.io/artifactory/arrow/ubuntu $CODENAME main" \
     | sudo tee /etc/apt/sources.list.d/apache-arrow.list

4. sudo gpg --no-default-keyring \
  --keyring /usr/share/keyrings/apache-arrow-archive-keyring.gpg \
  --keyserver keyserver.ubuntu.com \
  --recv-keys 9CBA4EF977CA20B8

  To verify: 
  sudo gpg --no-default-keyring \
  --keyring /usr/share/keyrings/apache-arrow-archive-keyring.gpg \
  --list-keys --keyid-format LONG
  must see: key 9CBA4EF977CA20B8
5. sudo apt-get update
   sudo apt-get install -y \
     libarrow-dev \
     libparquet-dev

### To launch Claude CLI
## install nvm
% curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.39.7/install.sh | bash
% export NVM_DIR="$HOME/.nvm"
% [ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"
% [ -s "$NVM_DIR/bash_completion" ] && \. "$NVM_DIR/bash_completion"
% nvm install 22
% nvm use 22
% nvm alias default 22
% npm install -g @anthropic-ai/claude-code
% claude

### To use Arrow
% sudo apt-get update
% sudo apt-get install -y \
  git cmake ninja-build g++ \
  zlib1g-dev liblz4-dev libzstd-dev libsnappy-dev \
  libssl-dev
% git clone https://github.com/apache/arrow.git
% cd arrow/cpp; mkdir -p build; cd build
% cmake .. \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DARROW_BUILD_SHARED=ON \
  -DARROW_BUILD_STATIC=OFF \
  -DARROW_PARQUET=OFF \
  -DARROW_CSV=ON \
  -DARROW_JSON=ON
% ninja -j"$(nproc)"
% sudo ninja install