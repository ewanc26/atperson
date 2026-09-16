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
  src/core/action.c
  src/core/bootstrap.c
  src/core/neural.c
  src/core/ledger.c
  src/core/persistence.c)
target_include_directories(
  atperson_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_features(atperson_core PUBLIC c_std_23)

if(UNIX)
  target_compile_definitions(atperson_core PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(atperson_core PRIVATE m)
endif()

add_library(atperson_cpp src/app/graph.cpp src/app/ledger.cpp)
target_include_directories(atperson_cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atperson_cpp PUBLIC atperson_core)
target_compile_features(atperson_cpp PUBLIC cxx_std_23)

# Runtime ingestion state is JSON; cJSON is the same parser Wolfram uses.
# Fetch it unconditionally so the runtime layer builds and tests without the
# network stack too. (cJSON v1.7.18 declares an old cmake_minimum_required;
# the policy floor keeps it configurable with modern CMake, same workaround
# Wolfram's own build uses.)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18)
set(WOLFRAM_CJSON_OVERRIDDEN
    OFF
    CACHE INTERNAL "")
# cJSON's own test suite compiles cJSON.c with -Werror-incompatible settings
# and is not needed; atperson's tests exercise the parser through real usage.
set(ENABLE_CJSON_TEST
    OFF
    CACHE INTERNAL "")
FetchContent_MakeAvailable(cjson)
# cJSON v1.7.18 declares old cmake_minimum_required and defines a `bool`
# macro that Clang rejects under -Werror; same workarounds Wolfram's build
# uses.
target_compile_options(cjson PRIVATE -Wno-keyword-macro)
# cJSON's own CMakeLists does not declare target include directories; the
# header lives at the top of its source tree (same as Wolfram's build).
target_include_directories(atperson_cpp PUBLIC ${cjson_SOURCE_DIR})
if(NOT WOLFRAM_CJSON_OVERRIDDEN)
  target_link_libraries(atperson_cpp PUBLIC cjson)
endif()

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
  # cJSON is already fetched above; tell Wolfram's build to reuse it rather
  # than declaring a second FetchContent of the same name.
  set(WOLFRAM_CJSON_OVERRIDDEN
      ON
      CACHE INTERNAL "")
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
    src/app/atproto_client.cpp
    src/app/ingestion_policy.cpp
    src/app/ingestion_state.cpp
    src/app/state_lock.cpp
    src/app/sync_engine.cpp)
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

  add_executable(atperson_memory_test tests/memory_test.c)
  target_link_libraries(atperson_memory_test PRIVATE atperson_core)
  add_test(NAME atperson_memory COMMAND atperson_memory_test)

  add_executable(atperson_familiarity_test tests/familiarity_test.c)
  target_link_libraries(atperson_familiarity_test PRIVATE atperson_core)
  add_test(NAME atperson_familiarity COMMAND atperson_familiarity_test)

  add_executable(atperson_action_test tests/action_test.c)
  target_link_libraries(atperson_action_test PRIVATE atperson_core)
  add_test(NAME atperson_action COMMAND atperson_action_test)

  # The ledger test binary is re-invoked as *separate OS processes* so the
  # cross-run dedup test exercises a genuinely restarted process. Phase 1
  # commits entries for several posts; phase 2 (which CTest runs afterwards)
  # re-opens the same files and asserts the unique (source id + digest) index
  # blocks every already-committed post.
  add_executable(atperson_ledger_test tests/ledger_test.c)
  target_link_libraries(atperson_ledger_test PRIVATE atperson_core)
  add_test(
    NAME atperson_ledger_dedup_phase1
    COMMAND atperson_ledger_test dedup-phase1
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-runs)
  add_test(
    NAME atperson_ledger_dedup_phase2
    COMMAND atperson_ledger_test dedup-phase2
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-runs)
  set_tests_properties(atperson_ledger_dedup_phase2
                       PROPERTIES DEPENDS atperson_ledger_dedup_phase1)
  add_test(
    NAME atperson_ledger_roundtrip
    COMMAND atperson_ledger_test roundtrip
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-roundtrip)
  add_test(
    NAME atperson_ledger_crash_safety
    COMMAND atperson_ledger_test crash-safety
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-crash)
  add_test(
    NAME atperson_ledger_payloads
    COMMAND atperson_ledger_test payloads
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-payloads)
  add_test(
    NAME atperson_ledger_payload_corruption
    COMMAND atperson_ledger_test payload-corruption
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-payload-corruption)
  add_test(
    NAME atperson_ledger_migration
    COMMAND atperson_ledger_test migration
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-migration)

  add_executable(atperson_cpp_test tests/cpp_test.cpp)
  target_link_libraries(atperson_cpp_test PRIVATE atperson_cpp)
  add_test(NAME atperson_cpp COMMAND atperson_cpp_test)

  # Ingestion-state format and sync-engine restart semantics; offline
  # (fixture page fetchers, no live network).
  add_executable(
    atperson_sync_test
    tests/sync_test.cpp
    src/app/ingestion_policy.cpp
    src/app/ingestion_state.cpp
    src/app/sync_engine.cpp)
  target_link_libraries(atperson_sync_test PRIVATE atperson_cpp)
  target_include_directories(atperson_sync_test PRIVATE src/app)
  add_test(NAME atperson_sync COMMAND atperson_sync_test)

  # Ingestion policy: every skip class produces the documented reason and
  # eligible classes are tagged (repost/reply). Offline, no network.
  add_executable(
    atperson_policy_test
    tests/policy_test.cpp
    src/app/ingestion_policy.cpp)
  target_compile_features(atperson_policy_test PRIVATE cxx_std_23)
  target_include_directories(atperson_policy_test PRIVATE src/app)
  add_test(NAME atperson_policy COMMAND atperson_policy_test)

  add_executable(
    atperson_lock_test
    tests/lock_test.cpp
    src/app/state_lock.cpp)
  target_compile_features(atperson_lock_test PRIVATE cxx_std_23)
  target_include_directories(atperson_lock_test PRIVATE src/app)
  add_test(NAME atperson_lock COMMAND atperson_lock_test)

  add_executable(atperson_bootstrap_test tests/bootstrap_test.c)
  target_link_libraries(atperson_bootstrap_test PRIVATE atperson_core)
  if(UNIX)
    target_compile_definitions(atperson_bootstrap_test PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  add_test(NAME atperson_bootstrap COMMAND atperson_bootstrap_test)
endif()
