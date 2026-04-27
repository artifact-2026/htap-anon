cmake_minimum_required(VERSION 3.16)

# ── Google Test ───────────────────────────────────────────────────────────────
# When built as part of the HTAP project, the parent already added the bundled
# gtest target via src/mycelium/third-party/gtest-1.8.1.  When built standalone
# fall back to the same bundled source or a system installation.
if(NOT TARGET gtest)
  set(_GTEST_FUSED
      "${CMAKE_CURRENT_SOURCE_DIR}/../../mycelium/third-party/gtest-1.8.1/fused-src/gtest")
  if(EXISTS "${_GTEST_FUSED}/CMakeLists.txt")
    add_subdirectory("${_GTEST_FUSED}" "${CMAKE_CURRENT_BINARY_DIR}/gtest")
  else()
    find_package(GTest REQUIRED)
    add_library(gtest ALIAS GTest::gtest)
  endif()
endif()

set(_GTEST_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../mycelium/third-party/gtest-1.8.1/fused-src")

# ── Helper macro ─────────────────────────────────────────────────────────────
macro(mycelium_test name)
  add_executable(${name} ${name}.cc)
  target_link_libraries(${name} PRIVATE mycelium::mycelium gtest)
  target_include_directories(${name} PRIVATE "${_GTEST_INCLUDE}")
  add_test(NAME ${name} COMMAND ${name})
endmacro()

# ── Test targets ─────────────────────────────────────────────────────────────
mycelium_test(test_ewma_admission_policy)
mycelium_test(test_transform_scheduler)
mycelium_test(test_compaction_slack_estimator)
mycelium_test(test_transform_epoch_tracker)
