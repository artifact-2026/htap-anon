#!/usr/bin/env bash

rm ../../test_results/mynoop/*
rm ../../test_results/baseline/*

ycsb_cmd=( ./src/test/ycsb/ycsb_test
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

print '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
print 'IDENTITY Transformation with input data format: FixedBin64'
"${ycsb_cmd[@]}"

ycsb_cmd2=( ./src/test/ycsb/ycsb_test
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

print '$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$'
print 'Baseline with input data format: FixedBin64'
"${ycsb_cmd2[@]}"