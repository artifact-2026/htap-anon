#!/bin/bash

./load_script.sh baseline false run b > point_query.txt
./load_script.sh cracking false run b >> point_query.txt
./load_script.sh flatbuffers false run b >> point_query.txt
./load_script.sh crackfb false run b >> point_query.txt

