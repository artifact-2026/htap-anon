add_definitions(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DFLATBUFF_INCLUDE=$FLATBUFF_INC)

set(ycsb_srcs
    ycsbc.cc
    db/db_factory.cc
    db/db_helper.cc
    db/test_rocks_db.cc
    db/test_splitting.cc
    db/test_indexing.cc
    db/test_precracking.cc
    db/test_converting.cc
    db/test_fb_cracker.cc
    db/test_preconverting.cc
    db/test_preindexing.cc
    db/test_crackplus.cc
    db/test_mynoop.cc
    core/core_workload.cc)
add_executable(ycsb_test ${ycsb_srcs})
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/test/ycsb)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium/include)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium)
target_link_libraries(
    ycsb_test
    rocksdb
    ${Protobuf_LIBRARIES}
    proto
    ${FLATBUFFERS_LIBRARY}
)
