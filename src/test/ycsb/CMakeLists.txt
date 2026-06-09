add_definitions(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DFLATBUFF_INCLUDE=$FLATBUFF_INC)

set(ycsb_srcs
    ycsbc.cc
    db/db_factory.cc
    db/db_helper.cc
    db/compaction_metrics_listener.cc
    db/test_rocks_db.cc
    db/test_splitting.cc
    db/test_indexing.cc
    db/test_precracking.cc
    db/test_converting.cc
    db/test_fb_cracker.cc
    db/test_preconverting.cc
    db/test_preindexing.cc
    db/test_mynoop.cc
    core/core_workload.cc)
add_executable(ycsb_test ${ycsb_srcs})
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/test/ycsb)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR} "${PROTO_GEN_DIR}")
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium/include)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium)
target_link_libraries(
    ycsb_test
    rocksdb
    ${Protobuf_LIBRARIES}
    proto_generated
    flatbuffers_generated
    ${FLATBUFFERS_LIBRARY}
)

# split_job: standalone two-phase resource-usage experiment for the
# "split rows into column groups" transformation (jobs/split.cpp). It
# generates synthetic rows the same way the YCSB DB backends do, spills them
# to flat data files, then reads them back and runs them through the real
# Mycelium Distributor transform -- all without touching RocksDB, so an
# external resource monitor (see the slack-meter project) can attribute
# CPU / I/O / RAM cost to the transform step in isolation.
set(split_job_srcs
    jobs/split.cpp
    core/core_workload.cc)
add_executable(split_job ${split_job_srcs})
target_include_directories(split_job PUBLIC ${CMAKE_SOURCE_DIR}/src/test/ycsb)
target_include_directories(split_job PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(split_job PUBLIC ${CMAKE_SOURCE_DIR} "${PROTO_GEN_DIR}")
target_include_directories(split_job PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium/include)
target_include_directories(split_job PUBLIC ${CMAKE_SOURCE_DIR}/src/mycelium)
target_link_libraries(
    split_job
    mycelium::mycelium
    ${Protobuf_LIBRARIES}
    proto_generated
)
