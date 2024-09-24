#!/bin/bash

# Function to print usage
usage() {
    echo "Usage: $0 [options] test_type load_only"
    echo "Options:"
    echo "  -h, --help          Show this help message and exit"
    echo "Arguments:"
    echo "  test_type           values: [baseline|cracking|flatbuffers|fb-cracking|precracking]"
    echo "  load_only           values: [true|false]"
}

# Check for help option or missing arguments
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
fi

# Check for the correct number of arguments
if [ $# -ne 2 ]; then
    echo "Error: Invalid number of arguments"
    usage
    exit 1
fi

# Get the command line arguments
TEST_TYPE=$1
OUTPUT_DIRECTORY="test-$1"
LOAD_ONLY=$2

TEST_TYPE=$1
if [ "$TEST_TYPE" != "baseline" ] && \
   [ "$TEST_TYPE" != "cracking" ] && \
   [ "$TEST_TYPE" != "flatbuffers" ] && \
   [ "$TEST_TYPE" != "crackfb" ] && \
   [ "$TEST_TYPE" != "precracking" ] && \
   [ "$TEST_TYPE" != "indexing" ]; then
   echo "TEST_TYPE is required with value = [baseline|cracking|flatbuffers|crackfb|precracking|indexing]"
   exit 1
fi

if [ "$LOAD_ONLY" != "true" ] && [ "$LOAD_ONLY" != "false" ]; then
  echo "LOAD_ONLY (3rd argument) needs to be boolean, but it is: $LOAD_ONLY"
  exit 1
fi

# Get the current directory
CURRENT_DIR=$(pwd)

# Directory path to check
TEST_RESULT_DIRECTORY="$CURRENT_DIR/src/test/ycsb/output/$OUTPUT_DIRECTORY"

# Check if the directory exists
if [ -d "$TEST_RESULT_DIRECTORY" ]; then
  if [ "$LOAD_ONLY" = "true" ]; then
    if [ "$(ls -A $TEST_RESULT_DIRECTORY)" ]; then
      echo "Clearing $TEST_RESULT_DIRECTORY since we are doing loading only" 
      rm $TEST_RESULT_DIRECTORY/*
    fi
  fi
else
  echo "Directory $TEST_RESULT_DIRECTORY does not exist. Creating it now..."
  mkdir -p "$TEST_RESULT_DIRECTORY"
  if [ $? -eq 0 ]; then
    echo "Directory $TEST_RESULT_DIRECTORY created successfully."
  else
    echo "Failed to create directory $TEST_RESULT_DIRECTORY."
  fi
fi

if [ "$LOAD_ONLY" = "true" ]; then
  echo "Loading $TEST_TYPE in $TEST_RESULT_DIRECTORY ..."
fi

if [ "$TEST_TYPE" = "baseline" ] || [ "$TEST_TYPE" = "precracking" ]; then
  ./src/test/ycsb/ycsb_test -db $TEST_TYPE -dbpath $TEST_RESULT_DIRECTORY \
    -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap $LOAD_ONLY -threads 2 \
    -load true -run false -throughput false -levels 6
elif [ "$TEST_TYPE" = "cracking" ]; then
  ./src/test/ycsb/ycsb_test -db $TEST_TYPE -dbpath $TEST_RESULT_DIRECTORY \
    -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap $LOAD_ONLY -threads 2 \
    -load true -run false -throughput false -levels 6 -table $TEST_TYPE
elif [ "$TEST_TYPE" = "flatbuffers" ]; then
  ./src/test/ycsb/ycsb_test -db $TEST_TYPE -dbpath $TEST_RESULT_DIRECTORY \
    -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap $LOAD_ONLY -threads 2 \
    -load true -run false -throughput false -levels 6 -table $TEST_TYPE
elif [ "$TEST_TYPE" = "crackfb" ]; then
  ./src/test/ycsb/ycsb_test -db $TEST_TYPE -dbpath $TEST_RESULT_DIRECTORY \
    -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap $LOAD_ONLY -threads 2 \
    -load true -run false -throughput false -levels 6 -table $TEST_TYPE
elif [ "$TEST_TYPE" = "indexing" ]; then
  ./src/test/ycsb/ycsb_test -db $TEST_TYPE -dbpath $TEST_RESULT_DIRECTORY \
    -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap $LOAD_ONLY -threads 2 \
    -load true -run false -throughput false -levels 6 -table $TEST_TYPE
  fi
# ./src/test/ycsb/ycsb_test -db rocksdb -dbpath /holly/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false
# ./src/test/ycsb/ycsb_test -db rocksdb_column_strawman -dbpath /holly/test_result/test-precracking -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false
# ./src/test/ycsb/ycsb_test -db writetwice -dbpath /holly/test_result/test-writetwice -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -table writetwice


#for i in {1..2}
#do
#    ./src/test/ycsb/ycsb_test -db mycelium -dbpath /holly/test_result/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true

#    ./src/test/ycsb/ycsb_test -db rocksdb -dbpath /holly/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

#    ./src/test/ycsb/ycsb_test -db rocksdb_column_strawman -dbpath /holly/test_result/test-precracking -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform false

#    ./src/test/ycsb/ycsb_test -db writetwice -dbpath /holly/test_result/test-writetwice -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap false -threads 16 -load false -run false -throughput true -throughputtype 2 -runtime 300 -transform true -table writetwice

#done
