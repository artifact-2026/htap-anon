add_definitions(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX)

# ── Helper macro: integration tests that link against the full RocksDB+Mycelium stack ──
macro(mycelium_integration_test name)
  add_executable(${name} ${name}.cc)
  target_include_directories(${name} PRIVATE
      ${CMAKE_SOURCE_DIR}/src/mycelium/include
      ${CMAKE_SOURCE_DIR}/src/mycelium
      ${CMAKE_SOURCE_DIR}/src/libmycelium/include
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}
      "${PROTO_GEN_DIR}"
      ${CMAKE_SOURCE_DIR}/src/mycelium/third-party/gtest-1.8.1/fused-src)
  target_link_libraries(${name} PRIVATE
      rocksdb
      mycelium::mycelium
      gtest
      ${Protobuf_LIBRARIES}
      proto_generated
      flatbuffers_generated
      ${FLATBUFFERS_LIBRARY})
  add_test(NAME ${name} COMMAND ${name})
endmacro()

# ── Integration test targets ──────────────────────────────────────────────────

# Track B #5: bottommost compaction source-xor-dest invariant
mycelium_integration_test(test_bottommost_compaction)

# E16: level-eligibility once-only correctness gate
#   Verifies SPLIT fires exactly once per (source SST, dest CF) pair;
#   epoch property persists across re-compactions; recursive SPLIT tree integrity.
mycelium_integration_test(test_level_eligibility)

# E12: tombstone / delete consistency gate (R2 W5)
#   Verifies deleted keys are absent from all derived CFs after compaction;
#   covers point-delete, mixed ops, and DeleteRange for SPLIT and IDENTITY transforms.
mycelium_integration_test(test_delete_consistency)
