#!/bin/bash

./load_script.sh baseline true load a > load_test.txt
./load_script.sh cracking true load a >> load_test.txt
./load_script.sh flatbuffers true load a >> load_test.txt
./load_script.sh indexing true load a >> load_test.txt
./load_script.sh crackfb true load a >> load_test.txt
./load_script.sh precracking true load a >> load_test.txt
./load_script.sh preconverting true load a >> load_test.txt
./load_script.sh preindexing true load a >> load_test.txt


# point queries
./load_script.sh baseline false run b > point_query.txt
./load_script.sh cracking false run b >> point_query.txt
./load_script.sh flatbuffers false run b >> point_query.txt
./load_script.sh crackfb false run b >> point_query.txt