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
  src/core/memory.c
  src/core/action.c
  src/core/decision.c
  src/core/context.c
  src/core/bootstrap.c
  src/core/neural.c
  src/core/ledger.c
  src/core/ledger_format.c
  src/core/ledger_off.c
  src/core/ledger_index.c
  src/core/ledger_recover.c
  src/core/replay.c
  src/core/tokenize.c
  src/core/portable_io.c
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

add_library(
  atperson_cpp
  src/app/graph.cpp
  src/app/ledger.cpp
  src/app/system_resources.cpp
  src/app/system_filesystems.cpp
  src/app/resource_budget.cpp
  src/app/resource_runtime.cpp)
target_include_directories(
  atperson_cpp
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(atperson_cpp PUBLIC atperson_core)
target_compile_features(atperson_cpp PUBLIC cxx_std_23)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include(FetchContent)
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18)
set(WOLFRAM_CJSON_OVERRIDDEN
    OFF
    CACHE INTERNAL "")
set(ENABLE_CJSON_TEST
    OFF
    CACHE INTERNAL "")
FetchContent_MakeAvailable(cjson)
target_compile_options(cjson PRIVATE -Wno-keyword-macro)
target_include_directories(atperson_cpp PUBLIC ${cjson_SOURCE_DIR})
if(NOT WOLFRAM_CJSON_OVERRIDDEN)
  target_link_libraries(atperson_cpp PUBLIC cjson)
endif()

FetchContent_Declare(
  utf8proc
  GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
  GIT_TAG v2.11.0)
set(UTF8PROC_INSTALL OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(utf8proc)
target_link_libraries(atperson_core PUBLIC utf8proc)

if(MSVC)
  target_compile_options(atperson_core PRIVATE /W4)
  target_compile_options(atperson_cpp PRIVATE /W4)
else()
  target_compile_options(atperson_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(atperson_cpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ATPERSON_BUILD_NETWORK)
  include(FetchContent)

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

  add_executable(atperson_action_decision_test tests/action_decision_test.c)
  target_link_libraries(atperson_action_decision_test PRIVATE atperson_core)
  add_test(NAME atperson_action_decision COMMAND atperson_action_decision_test)

  add_executable(atperson_context_test tests/context_test.c)
  target_link_libraries(atperson_context_test PRIVATE atperson_core)
  add_test(NAME atperson_context COMMAND atperson_context_test)

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

  add_executable(atperson_replay_test tests/replay_test.c)
  target_link_libraries(atperson_replay_test PRIVATE atperson_core)
  add_test(
    NAME atperson_replay
    COMMAND atperson_replay_test replay ${CMAKE_CURRENT_BINARY_DIR}/replay)
  add_test(
    NAME atperson_replay_rebuild_safety
    COMMAND atperson_replay_test rebuild-safety
            ${CMAKE_CURRENT_BINARY_DIR}/replay-safety)

  add_executable(atperson_withdraw_test tests/withdraw_test.c)
  target_link_libraries(atperson_withdraw_test PRIVATE atperson_core)
  add_test(
    NAME atperson_withdraw
    COMMAND atperson_withdraw_test withdraw ${CMAKE_CURRENT_BINARY_DIR}/withdraw)

  add_executable(atperson_compact_test tests/compact_test.c)
  target_link_libraries(atperson_compact_test PRIVATE atperson_core)
  add_test(
    NAME atperson_compact
    COMMAND atperson_compact_test compact ${CMAKE_CURRENT_BINARY_DIR}/compact)
  add_test(
    NAME atperson_compact_rebuild_equivalence
    COMMAND atperson_compact_test rebuild-equivalence
            ${CMAKE_CURRENT_BINARY_DIR}/compact-rebuild)

  add_executable(atperson_tokenize_test tests/tokenize_test.c)
  target_link_libraries(atperson_tokenize_test PRIVATE atperson_core)
  target_include_directories(atperson_tokenize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  add_test(
    NAME atperson_tokenize
    COMMAND atperson_tokenize_test tokenize)
  add_test(
    NAME atperson_tokenize_graph
    COMMAND atperson_tokenize_test tokenize-graph)

  add_executable(atperson_growth_test tests/growth_test.c)
  target_link_libraries(atperson_growth_test PRIVATE atperson_core)
  add_test(
    NAME atperson_growth
    COMMAND atperson_growth_test growth)
  add_test(
    NAME atperson_growth_pathology
    COMMAND atperson_growth_test growth-pathology)

  add_executable(atperson_bench tests/bench.c)
  target_link_libraries(atperson_bench PRIVATE atperson_core)
  add_test(
    NAME atperson_bench
    COMMAND atperson_bench bench)
  set_tests_properties(atperson_bench PROPERTIES LABELS "bench")

  add_executable(atperson_snapshot_test tests/snapshot_test.c)
  target_link_libraries(atperson_snapshot_test PRIVATE atperson_core)
  add_test(
    NAME atperson_snapshot
    COMMAND atperson_snapshot_test)

  add_executable(atperson_cpp_test tests/cpp_test.cpp)
  target_link_libraries(atperson_cpp_test PRIVATE atperson_cpp)
  add_test(NAME atperson_cpp COMMAND atperson_cpp_test)

  add_executable(atperson_resource_test tests/resource_test.cpp)
  target_link_libraries(atperson_resource_test PRIVATE atperson_cpp)
  target_include_directories(atperson_resource_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
  add_test(NAME atperson_resource COMMAND atperson_resource_test)

  add_executable(
    atperson_sync_test
    tests/sync_test.cpp
    src/app/ingestion_policy.cpp
    src/app/ingestion_state.cpp
    src/app/sync_engine.cpp)
  target_link_libraries(atperson_sync_test PRIVATE atperson_cpp)
  target_include_directories(atperson_sync_test PRIVATE src/app)
  add_test(NAME atperson_sync COMMAND atperson_sync_test)

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
