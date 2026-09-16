cmake_minimum_required(VERSION 3.24)

project(
  atperson
  VERSION 0.1.0
  DESCRIPTION "A persistent learning entity for the AT Protocol"
  LANGUAGES C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ATPERSON_BUILD_NETWORK "Build the Wolfram-backed AT Protocol runtime" ON)
option(ATPERSON_BUILD_TESTS "Build atperson tests" ON)

add_library(
  atperson_core
  src/core/graph.c
  src/core/neural.c
  src/core/persistence.c)
target_include_directories(
  atperson_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_features(atperson_core PUBLIC c_std_23)

if(UNIX)
  target_link_libraries(atperson_core PRIVATE m)
endif()

add_library(atperson_cpp src/app/graph.cpp)
target_include_directories(atperson_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atperson_cpp PUBLIC atperson_core)
target_compile_features(atperson_cpp PUBLIC cxx_std_23)

if(MSVC)
  target_compile_options(atperson_core PRIVATE /W4)
  target_compile_options(atperson_cpp PRIVATE /W4)
else()
  target_compile_options(atperson_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(atperson_cpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ATPERSON_BUILD_NETWORK)
  include(FetchContent)

  # Keep Wolfram as a dependency rather than duplicating AT Protocol logic here.
  # Pin the current known-good revision so a clean build is reproducible.
  set(WOLFRAM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WOLFRAM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WOLFRAM_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(WOLFRAM_BUILD_CPP ON CACHE BOOL "" FORCE)

  FetchContent_Declare(
    wolfram
    GIT_REPOSITORY https://github.com/ewanc26/wolfram.git
    GIT_TAG 9e63f76ab0b4f97f2cb5c62a5d0129b3d9023917
    # wolfram-src/cpp/CMakeLists.txt uses ${CMAKE_SOURCE_DIR} (the top-level
    # project) to locate Wolfram's public headers. Under FetchContent that is
    # atperson, so the generated_owners.hpp codegen step would scan a
    # non-existent include/wolfram. Rewrite those references to the actual
    # Wolfram source dir.
    PATCH_COMMAND
      ${CMAKE_COMMAND} -DWOLFRAM_SRC_DIR=${FETCHCONTENT_BASE_DIR}/wolfram-src
      -DWOLFRAM_CPP_CMAKE=${FETCHCONTENT_BASE_DIR}/wolfram-src/cpp/CMakeLists.txt
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/wolfram-cpp-source-dir.cmake)
  FetchContent_MakeAvailable(wolfram)

  add_executable(
    atperson
    src/app/main.cpp
    src/app/atproto_client.cpp)
  target_link_libraries(atperson PRIVATE atperson_cpp wolfram-cpp)
  target_compile_features(atperson PRIVATE cxx_std_23)

  if(MSVC)
    target_compile_options(atperson PRIVATE /W4)
  else()
    target_compile_options(atperson PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(ATPERSON_BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_executable(atperson_core_test tests/core_test.c)
  target_link_libraries(atperson_core_test PRIVATE atperson_core)
  add_test(NAME atperson_core COMMAND atperson_core_test)

  add_executable(atperson_cpp_test tests/cpp_test.cpp)
  target_link_libraries(atperson_cpp_test PRIVATE atperson_cpp)
  add_test(NAME atperson_cpp COMMAND atperson_cpp_test)
endif()
