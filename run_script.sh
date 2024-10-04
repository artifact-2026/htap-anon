#!/bin/bash

./load_script.sh baseline true load b > load_test.txt
./load_script.sh cracking true load b >> load_test.txt
./load_script.sh flatbuffers true load b >> load_test.txt
./load_script.sh indexing true load b >> load_test.txt
./load_script.sh crackfb true load b >> load_test.txt
./load_script.sh precracking true load b >> load_test.txt
./load_script.sh preconverting true load b >> load_test.txt
./load_script.sh preindexing true load b >> load_test.txt
