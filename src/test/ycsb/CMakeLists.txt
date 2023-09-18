set(ycsb_srcs
    ycsbc.cc
    db/db_factory.cc
    db/test_cabin_db.cc
    db/test_rocks_db.cc
    db/mycelium.cc
    db/mycelium_row_strawman.cc
    db/mycelium_column_strawman.cc
    core/core_workload.cc)
add_executable(ycsb_test ${ycsb_srcs})
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src/test/ycsb)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ycsb_test PUBLIC ${CMAKE_SOURCE_DIR}/proto)
target_link_libraries(
    ycsb_test
    cabindb
    proto
    ${Protobuf_LIBRARIES}
)
