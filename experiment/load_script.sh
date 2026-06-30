#!/bin/bash

# Function to print usage
usage() {
    echo "Usage: $0 [options] test_type load_only"
    echo "Options:"
    echo "  -h, --help          Show this help message and exit"
    echo "Arguments:"
    echo "  db_type             values: [baseline|splitting|converting|fb-cracking|presplitting|preconverting|preindexing|indexing|mynoop]"
    echo "  if_bootstrap        values: [true|false]"
    echo "  test_type           values: [run|xputr|xputl]"
    echo "  workload_type       values: [a|b|c|d|e|f]"
    echo "  threads             values: [>=1]"
}

# Check for help option or missing arguments
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
fi

# Check for the correct number of arguments
if [ $# -lt 5 ]; then
    echo "Error: Invalid number of arguments"
    usage
    exit 1
fi

# Get the command line arguments
DB_TYPE=$1
if [ "$DB_TYPE" != "baseline" ] && \
   [ "$DB_TYPE" != "splitting" ] && \
   [ "$DB_TYPE" != "converting" ] && \
   [ "$DB_TYPE" != "crackfb" ] && \
   [ "$DB_TYPE" != "presplitting" ] && \
   [ "$DB_TYPE" != "preconverting" ] && \
   [ "$DB_TYPE" != "preindexing" ] && \
   [ "$DB_TYPE" != "crackplus" ] && \
   [ "$DB_TYPE" != "mynoop" ] && \
   [ "$DB_TYPE" != "indexing" ]; then
   echo "DB_TYPE is required with value = [
              baseline|splitting|converting|crackfb|presplitting|
              preconverting|preindexing|indexing|crackplus|mynoop]"
   exit 1
fi

OUTPUT_DIRECTORY="test-$1"
IF_BOOTSTRAP=$2
if [ "$IF_BOOTSTRAP" != "true" ] && [ "$IF_BOOTSTRAP" != "false" ]; then
  echo "if_bootstrap (3rd argument) needs to be boolean, but it is: $IF_BOOTSTRAP"
  exit 1
fi

TEST_TYPE=$3
RUN=false
XPUT=false
LOAD=false
XPUT_TYPE=1
if [ "$TEST_TYPE" = "run" ]; then
  RUN=true
elif [ "$TEST_TYPE" = "xputr" ]; then
  XPUT=true
  XPUT_TYPE=1
elif [ "$TEST_TYPE" = "xputl" ]; then
  XPUT=true
  XPUT_TYPE=2
elif [ "$TEST_TYPE" = "load" ]; then
  LOAD=true
fi

WORKLOAD_TYPE=$4
WORKLOAD="../src/test/ycsb/workloads/test_workload$WORKLOAD_TYPE.spec"

THREADS=$5

# Get the current directory
CURRENT_DIR=$(pwd)

# Directory path to check
TEST_RESULT_DIRECTORY="$CURRENT_DIR/src/test/ycsb/output/$OUTPUT_DIRECTORY"

# Load, Run, or Throughput

# Check if the directory exists
if [ -d "$TEST_RESULT_DIRECTORY" ]; then
  if [ "$IF_BOOTSTRAP" = "true" ]; then
    if [ "$(ls -A $TEST_RESULT_DIRECTORY)" ]; then
      echo "Clearing $TEST_RESULT_DIRECTORY since we are starting from scratch..." 
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

if [ "$IF_BOOTSTRAP" = "true" ]; then
  echo "Loading $DB_TYPE in $TEST_RESULT_DIRECTORY ..."
fi

./src/test/ycsb/ycsb_test -db $DB_TYPE -dbpath $TEST_RESULT_DIRECTORY \
  -P $WORKLOAD -bootstrap $IF_BOOTSTRAP -threads $THREADS \
  -load $LOAD -run $RUN -throughput $XPUT -levels 6 -table $DB_TYPE \
  -throughputtype $XPUT_TYPE
