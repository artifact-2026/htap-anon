echo "xput test" > xput_test.txt
./load_script.sh baseline true load a 4 2 >> xput_test.txt
./load_script.sh baseline false xputl a 4 2 >> xput_test.txt
./load_script.sh cracking true load a 4 2 >> xput_test.txt
./load_script.sh cracking false xputl a 4 2 >> xput_test.txt
./load_script.sh flatbuffers true load a 4 2 >> xput_test.txt
./load_script.sh flatbuffers false xputl a 4 2 >> xput_test.txt
./load_script.sh indexing true load a 4 2 >> xput_test.txt
./load_script.sh indexing false xputl a 4 2 >> xput_test.txt
./load_script.sh crackfb true load a 4 2 >> xput_test.txt
./load_script.sh crackfb false xputl a 4 2 >> xput_test.txt
./load_script.sh crackplus true load a 4 2 >> xput_test.txt
./load_script.sh crackplus false xputl a 4 2 >> xput_test.txt