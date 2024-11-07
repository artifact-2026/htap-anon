#!/bin/bash

./load_script.sh baseline true load a 2 > load_test.txt
./load_script.sh cracking true load a 2 >> load_test.txt
./load_script.sh flatbuffers true load a 2 >> load_test.txt
./load_script.sh indexing true load a 2 >> load_test.txt
./load_script.sh crackfb true load a 2 >> load_test.txt
./load_script.sh precracking true load a 2 >> load_test.txt
./load_script.sh preconverting true load a 2 >> load_test.txt
./load_script.sh preindexing true load a 2 >> load_test.txt


# point queries
./load_script.sh baseline false xputr b 1 > point_query.txt
./load_script.sh cracking false xputr b >> point_query.txt
./load_script.sh flatbuffers false xputr b >> point_query.txt
./load_script.sh crackfb false xputr b >> point_query.txt