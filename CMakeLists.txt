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

set(ATPERSON_APP_PRIVATE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/action
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/atproto
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/audit
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ingestion
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/state
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/sync)

add_library(
  atperson_core
  src/core/graph/lifecycle.c
  src/core/graph/index.c
  src/core/graph/store.c
  src/core/graph/observe.c
  src/core/graph/query.c
  src/core/memory.c
  src/core/action/ordering.c
  src/core/action/candidates.c
  src/core/action/plans.c
  src/core/action/guard.c
  src/core/context.c
  src/core/bootstrap.c
  src/core/neural.c
  src/core/ledger/lifecycle.c
  src/core/ledger/commit.c
  src/core/ledger/append.c
  src/core/ledger/outcome.c
  src/core/ledger/query.c
  src/core/ledger/compact.c
  src/core/ledger/format.c
  src/core/ledger/off.c
  src/core/ledger/index.c
  src/core/ledger/recover.c
  src/core/replay.c
  src/core/tokenize.c
  src/core/portable_io.c
  src/core/persistence/encode.c
  src/core/persistence/reader.c
  src/core/persistence/decode.c
  src/core/persistence/migration.c
  src/core/persistence/load.c)
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
  src/app/action/inspection.cpp
  src/app/audit/evidence.cpp
  src/app/audit/questions.cpp
  src/app/audit/verdict.cpp
  src/app/resource/system.cpp
  src/app/resource/filesystems.cpp
  src/app/resource/budget.cpp
  src/app/resource/runtime.cpp)
target_include_directories(
  atperson_cpp
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
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

# Token identity is durable learning state (issue #8): the core needs a
# deterministic Unicode contract — NFKC_Casefold normalization, category-
# based token boundaries, UTF-8 validation. utf8proc is pure C (no C++ or
# Wolfram dependency), MIT-licensed, and is the same Unicode backend Julia
# uses. Pinned tag for reproducible builds.
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
    src/app/cli/config.cpp
    src/app/cli/usage.cpp
    src/app/cli/ledger_maintenance.cpp
    src/app/cli/graph_inspection.cpp
    src/app/cli/ingest.cpp
    src/app/cli/cursor.cpp
    src/app/cli/sync.cpp
    src/app/atproto/client.cpp
    src/app/audit/command.cpp
    src/app/audit/transport.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/state/lock.cpp
    src/app/sync/engine.cpp)
  target_include_directories(atperson PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
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

  # Deterministic replay/rebuild from the ledger; pure core, no network.
  add_executable(atperson_replay_test tests/replay_test.c)
  target_link_libraries(atperson_replay_test PRIVATE atperson_core)
  add_test(
    NAME atperson_replay
    COMMAND atperson_replay_test replay ${CMAKE_CURRENT_BINARY_DIR}/replay)
  add_test(
    NAME atperson_replay_rebuild_safety
    COMMAND atperson_replay_test rebuild-safety
            ${CMAKE_CURRENT_BINARY_DIR}/replay-safety)

  # Source withdrawal/unlearning semantics; pure core, no network.
  add_executable(atperson_withdraw_test tests/withdraw_test.c)
  target_link_libraries(atperson_withdraw_test PRIVATE atperson_core)
  add_test(
    NAME atperson_withdraw
    COMMAND atperson_withdraw_test withdraw ${CMAKE_CURRENT_BINARY_DIR}/withdraw)

  # Safe ledger compaction and checkpointing; pure core, no network.
  add_executable(atperson_compact_test tests/compact_test.c)
  target_link_libraries(atperson_compact_test PRIVATE atperson_core)
  add_test(
    NAME atperson_compact
    COMMAND atperson_compact_test compact ${CMAKE_CURRENT_BINARY_DIR}/compact)
  add_test(
    NAME atperson_compact_rebuild_equivalence
    COMMAND atperson_compact_test rebuild-equivalence
            ${CMAKE_CURRENT_BINARY_DIR}/compact-rebuild)

  # Unicode tokenization contract; pure core, no network.
  add_executable(atperson_tokenize_test tests/tokenize_test.c)
  target_link_libraries(atperson_tokenize_test PRIVATE atperson_core)
  target_include_directories(atperson_tokenize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  add_test(
    NAME atperson_tokenize
    COMMAND atperson_tokenize_test tokenize)
  add_test(
    NAME atperson_tokenize_graph
    COMMAND atperson_tokenize_test tokenize-graph)

  # Graph growth bounds and pathological input; pure core, no network.
  add_executable(atperson_growth_test tests/growth_test.c)
  target_link_libraries(atperson_growth_test PRIVATE atperson_core)
  add_test(
    NAME atperson_growth
    COMMAND atperson_growth_test growth)
  add_test(
    NAME atperson_growth_pathology
    COMMAND atperson_growth_test growth-pathology)

  # Scale benchmarks (issue #9). Explicit suite, excluded from default
  # runs via label; invoke with: ctest -L bench
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

  # Read-only rendering of authoritative C23 plan/context/decision traces.
  add_executable(atperson_action_inspection_test tests/action_inspection_test.cpp)
  target_link_libraries(atperson_action_inspection_test PRIVATE atperson_cpp)
  target_include_directories(atperson_action_inspection_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_action_inspection COMMAND atperson_action_inspection_test)

  # Advisory audit: evidence/state marshalling, question schema, and response
  # parsing/rendering are offline-testable; only the transport is excluded
  # from the ATPERSON_BUILD_NETWORK=OFF configuration.
  add_executable(atperson_audit_test tests/audit_test.cpp)
  target_link_libraries(atperson_audit_test PRIVATE atperson_cpp)
  target_include_directories(atperson_audit_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_audit COMMAND atperson_audit_test)

  # Dynamic host/container resource probing and deterministic budget policy.
  add_executable(atperson_resource_test tests/resource_test.cpp)
  target_link_libraries(atperson_resource_test PRIVATE atperson_cpp)
  target_include_directories(atperson_resource_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_resource COMMAND atperson_resource_test)

  # Ingestion-state format and sync-engine restart semantics; offline
  # (fixture page fetchers, no live network).
  add_executable(
    atperson_sync_test
    tests/sync_test.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/sync/engine.cpp)
  target_link_libraries(atperson_sync_test PRIVATE atperson_cpp)
  target_include_directories(atperson_sync_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_sync COMMAND atperson_sync_test)

  # Ingestion policy: every skip class produces the documented reason and
  # eligible classes are tagged (repost/reply). Offline, no network.
  add_executable(
    atperson_policy_test
    tests/policy_test.cpp
    src/app/ingestion/policy.cpp)
  target_compile_features(atperson_policy_test PRIVATE cxx_std_23)
  target_include_directories(atperson_policy_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_policy COMMAND atperson_policy_test)

  add_executable(
    atperson_lock_test
    tests/lock_test.cpp
    src/app/state/lock.cpp)
  target_compile_features(atperson_lock_test PRIVATE cxx_std_23)
  target_include_directories(atperson_lock_test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson_lock COMMAND atperson_lock_test)

  add_executable(atperson_bootstrap_test tests/bootstrap_test.c)
  target_link_libraries(atperson_bootstrap_test PRIVATE atperson_core)
  if(UNIX)
    target_compile_definitions(atperson_bootstrap_test PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  add_test(NAME atperson_bootstrap COMMAND atperson_bootstrap_test)
endif()
