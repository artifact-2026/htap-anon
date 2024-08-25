cmake_minimum_required(VERSION 3.16.3)

project(HTAP)

SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++17")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -D FLATBUFFERS_CXX_FLAGS=-Wno-error")

FIND_PACKAGE(Protobuf REQUIRED)
FIND_PACKAGE(Arrow REQUIRED)

add_subdirectory(lib/rocksdb)
add_subdirectory(src/test/ycsb)
add_subdirectory(proto)

include_directories(${CMAKE_SOURCE_DIR}/lib/rocksdb/include)
include_directories(/usr/include)
include_directories(${protobuf_INCLUDE_DIR})
include_directories(${FLATBUFFERS_INCLUDE_DIRS})

set(BUILD_DIR ${CMAKE_BINARY_DIR})

# Copy run_script.sh from the source directory to the build directory
configure_file(run_script.sh ${BUILD_DIR}/run_script.sh COPYONLY)

#set(ROCKSDB_INCLUDE_DIR /lib/rocksdb/include/rocksdb/)
#set(ROCKSDB_LIB_DIR /lib/rocksdb)

#target_include_directories(insert PUBLIC "${ROCKSDB_INCLUDE_DIR}")
#target_link_directories(insert PUBLIC "${ROCKSDB_LIB_DIR}")
