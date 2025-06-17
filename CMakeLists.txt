cmake_minimum_required(VERSION 3.16.3)

project(HTAP)

SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++17")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -D FLATBUFFERS_CXX_FLAGS=-Wno-error -frtti")

FIND_PACKAGE(Protobuf REQUIRED)

add_subdirectory(src/mycelium)
add_subdirectory(src/test/ycsb)
add_subdirectory(proto)

include_directories(${CMAKE_SOURCE_DIR}/src/mycelium/include)
include_directories(/usr/include)
include_directories(${protobuf_INCLUDE_DIR})
include_directories(${FLATBUFFERS_INCLUDE_DIRS})

set(BUILD_DIR ${CMAKE_BINARY_DIR})

# Copy run_script.sh from the source directory to the build directory
configure_file(utility/run_script.sh ${BUILD_DIR}/run_script.sh COPYONLY)
configure_file(utility/load_script.sh ${BUILD_DIR}/load_script.sh COPYONLY)
configure_file(utility/queries.sh ${BUILD_DIR}/queries.sh COPYONLY)
configure_file(utility/xput.sh ${BUILD_DIR}/xput.sh COPYONLY)