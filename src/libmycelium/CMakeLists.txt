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
# Arrow is a hard dep: it is the in-memory IR for all transformations.
find_package(Arrow CONFIG REQUIRED)

# Normalise Arrow target name (CONFIG packages may export different aliases)
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
option(MYCELIUM_WITH_PROTOBUF   "Enable protobuf parser/encoder"   OFF)
option(MYCELIUM_WITH_FLATBUFFERS "Enable flatbuffers parser/encoder" OFF)

if(MYCELIUM_WITH_PROTOBUF)
  find_package(Protobuf REQUIRED)
endif()

if(MYCELIUM_WITH_FLATBUFFERS)
  find_package(Flatbuffers REQUIRED)
endif()

# ── Header-only interface target (P0 — no sources yet) ────────────────────────
# In P2 this becomes a proper STATIC/SHARED library once source files are moved
# in.  For now we expose only the include path so RocksDB can consume the new
# headers during the transition period.
add_library(mycelium INTERFACE)
add_library(mycelium::mycelium ALIAS mycelium)

target_include_directories(mycelium INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(mycelium INTERFACE Arrow::arrow)

if(MYCELIUM_WITH_PROTOBUF)
  target_compile_definitions(mycelium INTERFACE MYCELIUM_WITH_PROTOBUF)
  target_link_libraries(mycelium INTERFACE protobuf::libprotobuf)
endif()

if(MYCELIUM_WITH_FLATBUFFERS)
  target_compile_definitions(mycelium INTERFACE MYCELIUM_WITH_FLATBUFFERS)
  target_link_libraries(mycelium INTERFACE flatbuffers::flatbuffers)
endif()

target_compile_features(mycelium INTERFACE cxx_std_17)

# ── Tests (populated in P3+) ─────────────────────────────────────────────────
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── Installation ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS mycelium EXPORT myceliumTargets)
install(DIRECTORY include/mycelium
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT myceliumTargets
  FILE myceliumTargets.cmake
  NAMESPACE mycelium::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mycelium)
