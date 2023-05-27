set(example_srcs
    example.cc
)

add_executable(example ${example_srcs})
target_include_directories(example PUBLIC ${CMAKE_SOURCE_DIR}/lib/rocksdb/include)
target_include_directories(example PUBLIC ${CMAKE_SOURCE_DIR}/lib/rocksdb)
target_include_directories(cabindb PUBLIC /usr/include)

target_link_libraries(example
                      rocksdb)