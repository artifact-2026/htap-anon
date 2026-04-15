#!/usr/bin/env bash

rm ../../test_results/baseline/*
rm ../../test_results/mynoop/*
rm ../../test_results/converting/*
rm ../../test_results/splitting/*
rm ../../test_results/indexing/*

ycsb_cmd=( ./src/test/ycsb/ycsb_test
  -db baseline
  -dbpath "/holly/test_results/baseline"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin64
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table baseline
)

echo '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
echo 'Baseline with input data format: JSON'
"${ycsb_cmd[@]}"

ycsb_cmd2=( ./src/test/ycsb/ycsb_test
  -db mynoop
  -dbpath "/holly/test_results/mynoop"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin64
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table mynoop
)

echo '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
echo 'IDENTITY Transformation with input data format: JSON'
"${ycsb_cmd2[@]}"

ycsb_cmd3=( ./src/test/ycsb/ycsb_test
  -db converting
  -dbpath "/holly/test_results/converting"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin64
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table converting
)

echo '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
echo 'CONVERT Transformation with input data format: JSON'
"${ycsb_cmd3[@]}"

ycsb_cmd4=( ./src/test/ycsb/ycsb_test
  -db splitting
  -dbpath "/holly/test_results/splitting"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin64
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table splitting
)

echo '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
echo 'SPLIT Transformation with input data format: JSON'
"${ycsb_cmd4[@]}"

ycsb_cmd5=( ./src/test/ycsb/ycsb_test
  -db indexing
  -dbpath "/holly/test_results/indexing"
  -P "../src/test/ycsb/workloads/test_workloada.spec"
  -inputdataformat fixedbin64
  -bootstrap true
  -threads 10
  -load true
  -run false
  -throughput false
  -table indexing
)

echo '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
echo 'INDEX Transformation with input data format: JSON'
"${ycsb_cmd5[@]}"
