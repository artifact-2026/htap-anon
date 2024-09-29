#!/bin/bash

./load_script.sh baseline true load > load_test.txt
./load_script.sh cracking true load >> load_test.txt
./load_script.sh flatbuffers true load >> load_test.txt
./load_script.sh indexing true load >> load_test.txt
./load_script.sh crackfb true load >> load_test.txt
./load_script.sh precracking true load >> load_test.txt
./load_script.sh preconverting true load >> load_test.txt
./load_script.sh preindexing true load >> load_test.txt
