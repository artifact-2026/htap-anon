#!/bin/bash

echo "loading baseline rowwidth=75" > load_data.txt
./load_script.sh baseline true load a_split 4 >> load_data.txt
./load_script.sh cracking true load a_split 4 >> load_data.txt

echo "running b1: " > query_result.txt
./load_script.sh baseline false run b1 1 >> query_result.txt
./load_script.sh cracking false run b1 1 >> query_result.txt

echo >> query_result.txt
echo "running b2: " >> query_result.txt
./load_script.sh baseline false run b2 1 >> query_result.txt
./load_script.sh cracking false run b2 1 >> query_result.txt

echo >> query_result.txt
echo "running b3: " >> query_result.txt
./load_script.sh baseline false run b3 1 >> query_result.txt
./load_script.sh cracking false run b3 1 >> query_result.txt

echo >> query_result.txt
echo "running b4: " >> query_result.txt
./load_script.sh baseline false run b4 1 >> query_result.txt
./load_script.sh cracking false run b4 1 >> query_result.txt

echo >> query_result.txt
echo "running c1: " >> query_result.txt
./load_script.sh baseline false run c1 1 >> query_result.txt
./load_script.sh cracking false run c1 1 >> query_result.txt

echo >> query_result.txt
echo "running c2: " >> query_result.txt
./load_script.sh baseline false run c2 1 >> query_result.txt
./load_script.sh cracking false run c2 1 >> query_result.txt

echo >> query_result.txt
echo "running c3: " >> query_result.txt
./load_script.sh baseline false run c3 1 >> query_result.txt
./load_script.sh cracking false run c3 1 >> query_result.txt

echo >> query_result.txt
echo "running c4: " >> query_result.txt
./load_script.sh baseline false run c4 1 >> query_result.txt
./load_script.sh cracking false run c4 1 >> query_result.txt

echo >> query_result.txt
echo "running d1: " >> query_result.txt
./load_script.sh baseline false run d1 1 >> query_result.txt
./load_script.sh cracking false run d1 1 >> query_result.txt

echo >> query_result.txt
echo "running d2: " >> query_result.txt
./load_script.sh baseline false run d2 1 >> query_result.txt
./load_script.sh cracking false run d2 1 >> query_result.txt

echo >> query_result.txt
echo "running d3: " >> query_result.txt
./load_script.sh baseline false run d3 1 >> query_result.txt
./load_script.sh cracking false run d3 1 >> query_result.txt

echo >> query_result.txt
echo "running d4: " >> query_result.txt
./load_script.sh baseline false run d4 1 >> query_result.txt
./load_script.sh cracking false run d4 1 >> query_result.txt

echo >> query_result.txt
echo "running e1: " >> query_result.txt
./load_script.sh baseline false run e1 1 >> query_result.txt
./load_script.sh cracking false run e1 1 >> query_result.txt

echo >> query_result.txt
echo "running e2: " >> query_result.txt
./load_script.sh baseline false run e2 1 >> query_result.txt
./load_script.sh cracking false run e2 1 >> query_result.txt

echo >> query_result.txt
echo "running e3: " >> query_result.txt
./load_script.sh baseline false run e3 1 >> query_result.txt
./load_script.sh cracking false run e3 1 >> query_result.txt

echo >> query_result.txt
echo "running e4: " >> query_result.txt
./load_script.sh baseline false run e4 1 >> query_result.txt
./load_script.sh cracking false run e4 1 >> query_result.txt

echo >> query_result.txt
echo "running a: " >> query_result.txt
./load_script.sh baseline false run a 1 >> query_result.txt
./load_script.sh cracking false run a 1 >> query_result.txt

echo >> query_result.txt
echo "running f: " >> query_result.txt
./load_script.sh baseline false run f 1 >> query_result.txt
./load_script.sh cracking false run f 1 >> query_result.txt