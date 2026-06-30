
FROM ubuntu:20.04

# this is for timezone config
ENV DEBIAN_FRONTEND=noninteractive 
ENV TZ=America/Los_Angeles
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt-get update
RUN apt-get install -y build-essential cmake git openssh-server g++ gdb pkg-config valgrind systemd-coredump libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev libzstd-dev liblz4-dev libprotobuf-dev protobuf-compiler ninja-build flatbuffers-compiler flatbuffers-compiler-dev libflatbuffers-dev librados-dev libradospp-dev
