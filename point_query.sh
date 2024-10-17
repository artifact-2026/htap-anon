#!/bin/bash

./load_script.sh baseline false xputr b 1 > point_query.txt
./load_script.sh cracking false xputr b 1 >> point_query.txt
./load_script.sh flatbuffers false xputr b 1 >> point_query.txt
./load_script.sh crackfb false xputr b 1 >> point_query.txt
./load_script.sh indexing false xputr b 1 >> point_query.txt
./load_script.sh precracking false xputr b 1 >> point_query.txt
./load_script.sh preconverting false xputr b 1 >> point_query.txt