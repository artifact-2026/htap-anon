cmake_minimum_required(VERSION 3.16.3)

project(HTAP)

SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++17")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -D FLATBUFFERS_CXX_FLAGS=-Wno-error")

FIND_PACKAGE(Protobuf REQUIRED)

add_subdirectory(lib/rocksdb)
add_subdirectory(src/cabindb)
add_subdirectory(src/test/ycsb)
add_subdirectory(src/example)
add_subdirectory(proto)

include_directories(${CMAKE_SOURCE_DIR}/lib/rocksdb/include)
include_directories(/usr/include)
include_directories(${protobuf_INCLUDE_DIR})
include_directories(${FLATBUFFERS_INCLUDE_DIRS})

#set(ROCKSDB_INCLUDE_DIR /lib/rocksdb/include/rocksdb/)
#set(ROCKSDB_LIB_DIR /lib/rocksdb)

#target_include_directories(insert PUBLIC "${ROCKSDB_INCLUDE_DIR}")
#target_link_directories(insert PUBLIC "${ROCKSDB_LIB_DIR}")
