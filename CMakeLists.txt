cmake_minimum_required(VERSION 3.16.3)

project(HTAP)

SET(CMAKE_CXX_FLAGS "-g -Wall -Werror -std=c++17")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -D FLATBUFFERS_CXX_FLAGS=-Wno-error -frtti")

find_package(Protobuf REQUIRED)
set(PROTO_SRC "${CMAKE_SOURCE_DIR}/ddl/proto/data.proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/proto_gen")

add_custom_command(
    OUTPUT
        "${PROTO_GEN_DIR}/data.pb.cc"
        "${PROTO_GEN_DIR}/data.pb.h"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS
        --proto_path=${CMAKE_SOURCE_DIR}/ddl/proto
        --cpp_out=${PROTO_GEN_DIR}
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    COMMENT "Generating data.pb.cc and data.pb.h into proto_gen"
)

add_custom_target(generate_proto_files
    DEPENDS "${PROTO_GEN_DIR}/data.pb.cc" "${PROTO_GEN_DIR}/data.pb.h"
)

add_library(proto_generated STATIC
    "${PROTO_GEN_DIR}/data.pb.cc"
    "${PROTO_GEN_DIR}/data.pb.h"
)
add_dependencies(proto_generated generate_proto_files)
target_include_directories(proto_generated PUBLIC "${PROTO_GEN_DIR}")

find_program(FLATC_EXECUTABLE flatc REQUIRED)
find_package(Flatbuffers REQUIRED)

set(FLATBUFFERS_SCHEMA ${CMAKE_SOURCE_DIR}/ddl/flat/row.fbs)
set(FLATBUFFERS_GEN_DIR ${CMAKE_BINARY_DIR}/flatbuffers_gen)

add_custom_command(
    OUTPUT
        ${FLATBUFFERS_GEN_DIR}/row_generated.h
    COMMAND ${FLATC_EXECUTABLE}
    ARGS -c --gen-object-api --reflect-types
         -o ${FLATBUFFERS_GEN_DIR}
         ${FLATBUFFERS_SCHEMA}
    DEPENDS ${FLATBUFFERS_SCHEMA}
    COMMENT "Generating FlatBuffers headers"
)

add_custom_target(generate_flatbuffers_files
    DEPENDS ${FLATBUFFERS_GEN_DIR}/row_generated.h
)

add_library(flatbuffers_generated INTERFACE)
add_dependencies(flatbuffers_generated generate_flatbuffers_files)

target_include_directories(flatbuffers_generated INTERFACE ${FLATBUFFERS_GEN_DIR})

add_subdirectory(src/mycelium)
add_subdirectory(src/test/ycsb)

include_directories(${CMAKE_SOURCE_DIR}/src/mycelium/include)
include_directories(/usr/include)
include_directories(${FLATBUFFERS_INCLUDE_DIRS})

set(BUILD_DIR ${CMAKE_BINARY_DIR})

# Copy run_script.sh from the source directory to the build directory
configure_file(utility/run_script.sh ${BUILD_DIR}/run_script.sh COPYONLY)
configure_file(utility/load_script.sh ${BUILD_DIR}/load_script.sh COPYONLY)
configure_file(utility/queries.sh ${BUILD_DIR}/queries.sh COPYONLY)
configure_file(utility/xput.sh ${BUILD_DIR}/xput.sh COPYONLY)
