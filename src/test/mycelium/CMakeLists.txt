add_definitions(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX)

add_executable(test_bottommost_compaction
    test_bottommost_compaction.cc)

target_include_directories(test_bottommost_compaction PRIVATE
    ${CMAKE_SOURCE_DIR}/src/mycelium/include
    ${CMAKE_SOURCE_DIR}/src/mycelium
    ${CMAKE_SOURCE_DIR}/src/libmycelium/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
    "${PROTO_GEN_DIR}"
    ${CMAKE_SOURCE_DIR}/src/mycelium/third-party/gtest-1.8.1/fused-src)

target_link_libraries(test_bottommost_compaction PRIVATE
    rocksdb
    mycelium::mycelium
    gtest
    ${Protobuf_LIBRARIES}
    proto_generated
    flatbuffers_generated
    ${FLATBUFFERS_LIBRARY})

add_test(NAME test_bottommost_compaction COMMAND test_bottommost_compaction)
