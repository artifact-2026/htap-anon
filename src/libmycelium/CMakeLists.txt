cmake_minimum_required(VERSION 3.16)
project(mycelium
  VERSION 0.1.0
  DESCRIPTION "Engine-agnostic LSM transformation core"
  LANGUAGES CXX)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Required dependencies ─────────────────────────────────────────────────────
find_package(Arrow CONFIG REQUIRED)

if(NOT TARGET Arrow::arrow)
  if(TARGET Arrow::arrow_shared)
    add_library(Arrow::arrow ALIAS Arrow::arrow_shared)
  elseif(TARGET Arrow::arrow_static)
    add_library(Arrow::arrow ALIAS Arrow::arrow_static)
  else()
    message(FATAL_ERROR "Arrow found but no library target exported")
  endif()
endif()

# ── Optional serialisation back-ends ─────────────────────────────────────────
option(MYCELIUM_WITH_PROTOBUF    "Enable protobuf parser/encoder"    OFF)
option(MYCELIUM_WITH_FLATBUFFERS "Enable flatbuffers parser/encoder" OFF)
option(MYCELIUM_WITH_JSON        "Enable JSON parser/encoder (jsoncpp)" OFF)

# ── P2: promoted from INTERFACE to STATIC ────────────────────────────────────
set(MYCELIUM_SOURCES
  # Core portable layer
  src/compaction_slack_estimator.cc
  src/transform_epoch_tracker.cc
  src/transform_scheduler.cc

  # Transformer implementations
  src/transformer/transformer_factory.cc
  src/transformer/distribute/distributor.cc
  src/transformer/convert/converter.cc
  src/transformer/augment/augmenter.cc
  src/transformer/identity/mynooper.cc

  # Parsers
  src/transformer/common/parser/csv_parser.cc
  src/transformer/common/parser/fixedbin64_parser.cc

  # Encoders
  src/transformer/common/encoder/csv_encoder.cc
  src/transformer/common/encoder/fixedbin64_encoder.cc
)

if(MYCELIUM_WITH_JSON OR (DEFINED jsoncpp_FOUND AND jsoncpp_FOUND))
  list(APPEND MYCELIUM_SOURCES
    src/transformer/common/parser/json_parser.cc
    src/transformer/common/encoder/json_encoder.cc)
endif()

if(MYCELIUM_WITH_FLATBUFFERS OR (DEFINED FLATBUFFERS_LIBRARY AND FLATBUFFERS_LIBRARY))
  list(APPEND MYCELIUM_SOURCES
    src/transformer/common/parser/flatbuffers_parser.cc
    src/transformer/common/encoder/flatbuffers_encoder.cc)
endif()

if(MYCELIUM_WITH_PROTOBUF OR (DEFINED Protobuf_FOUND AND Protobuf_FOUND))
  list(APPEND MYCELIUM_SOURCES
    src/transformer/common/parser/protobuf_parser.cc
    src/transformer/common/encoder/protobuf_encoder.cc)
endif()

add_library(mycelium STATIC ${MYCELIUM_SOURCES})
add_library(mycelium::mycelium ALIAS mycelium)

target_include_directories(mycelium
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    # Internal transformer headers live under src/transformer/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transformer
    # Generated protobuf / flatbuffers headers (set by parent CMakeLists)
    $<$<BOOL:${PROTO_GEN_DIR}>:${PROTO_GEN_DIR}>
    $<$<BOOL:${FLATBUFFERS_GEN_DIR}>:${FLATBUFFERS_GEN_DIR}>
)

target_link_libraries(mycelium
  PUBLIC  Arrow::arrow
  PRIVATE $<$<BOOL:${MYCELIUM_WITH_JSON}>:jsoncpp_lib>
          $<$<BOOL:${MYCELIUM_WITH_FLATBUFFERS}>:flatbuffers::flatbuffers>
          $<$<BOOL:${MYCELIUM_WITH_PROTOBUF}>:protobuf::libprotobuf>
)

target_compile_features(mycelium PUBLIC cxx_std_17)

# Pull in jsoncpp / flatbuffers / protobuf unconditionally when the parent
# already found them (we're a subdirectory of the RocksDB/htap build).
if(TARGET jsoncpp_lib)
  target_link_libraries(mycelium PRIVATE jsoncpp_lib)
endif()
if(TARGET flatbuffers_generated)
  target_link_libraries(mycelium PRIVATE flatbuffers_generated)
endif()
if(TARGET proto_generated)
  target_link_libraries(mycelium PRIVATE proto_generated)
endif()
if(DEFINED FLATBUFFERS_LIBRARY AND FLATBUFFERS_LIBRARY)
  target_link_libraries(mycelium PRIVATE ${FLATBUFFERS_LIBRARY})
endif()
if(DEFINED Protobuf_LIBRARIES AND Protobuf_LIBRARIES)
  target_link_libraries(mycelium PRIVATE ${Protobuf_LIBRARIES})
endif()

# ── Tests (populated in P3+) ─────────────────────────────────────────────────
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── Installation ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS mycelium EXPORT myceliumTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/mycelium
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT myceliumTargets
  FILE myceliumTargets.cmake
  NAMESPACE mycelium::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mycelium)
