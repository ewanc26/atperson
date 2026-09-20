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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/control
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/daemon
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ingestion
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/journal
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/outbound
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/state
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/sync)

add_library(
  atperson-core
  src/core/graph/lifecycle.c
  src/core/graph/index.c
  src/core/graph/groups.c
  src/core/graph/store.c
  src/core/graph/observe.c
  src/core/graph/query.c
  src/core/graph/valence.c
  src/core/memory.c
  src/core/action/ordering.c
  src/core/action/candidates.c
  src/core/action/plans.c
  src/core/action/guard.c
  src/core/context.c
  src/core/bootstrap.c
  src/core/neural_layout.c
  src/core/neural.c
  src/core/neural_migration.c
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
  src/core/ledger/migrate.c
  src/core/replay.c
  src/core/tokenize.c
  src/core/io/portable.c
  src/core/persistence/wire.c
  src/core/persistence/reader.c
  src/core/persistence/sections.c
  src/core/persistence/v5.c
  src/core/persistence/v6.c
  src/core/persistence/arch.c
  src/core/persistence/migration.c
  src/core/persistence/load.c
  src/core/persistence/save.c)
target_include_directories(
  atperson-core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_features(atperson-core PUBLIC c_std_23)

if(UNIX)
  target_compile_definitions(atperson-core PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(atperson-core PRIVATE m)
endif()

add_library(
  atperson-cpp
  src/app/graph.cpp
  src/app/ledger.cpp
  src/app/action/inspection.cpp
  src/app/audit/evidence.cpp
  src/app/audit/questions.cpp
  src/app/audit/verdict.cpp
  src/app/atproto/jetstream_filter.cpp
  src/app/resource/system.cpp
  src/app/resource/filesystems.cpp
  src/app/resource/budget.cpp
  src/app/resource/neural_runtime.cpp
  src/app/resource/runtime.cpp
  src/app/worker/pool.cpp)
target_include_directories(
  atperson-cpp
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
target_link_libraries(atperson-cpp PUBLIC atperson-core)
target_compile_features(atperson-cpp PUBLIC cxx_std_23)

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
target_include_directories(atperson-cpp PUBLIC ${cjson_SOURCE_DIR})
if(NOT WOLFRAM_CJSON_OVERRIDDEN)
  target_link_libraries(atperson-cpp PUBLIC cjson)
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
target_link_libraries(atperson-core PUBLIC utf8proc)

if(MSVC)
  target_compile_options(atperson-core PRIVATE /W4)
  target_compile_options(atperson-cpp PRIVATE /W4)
else()
  target_compile_options(atperson-core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(atperson-cpp PRIVATE -Wall -Wextra -Wpedantic)
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
    GIT_TAG 2b21221
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
    src/app/cli/maintenance.cpp
    src/app/cli/neural.cpp
    src/app/cli/inspection.cpp
    src/app/cli/ingest.cpp
    src/app/cli/control.cpp
    src/app/cli/cursor.cpp
    src/app/cli/daemon.cpp
    src/app/cli/jetstream.cpp
    src/app/cli/outbound.cpp
    src/app/cli/publish.cpp
    src/app/cli/sync.cpp
    src/app/atproto/client.cpp
    src/app/atproto/extract.cpp
  src/app/atproto/jetstream.cpp
  src/app/atproto/jetstream_client.cpp
  src/app/atproto/jetstream_replay.cpp
  src/app/atproto/jetstream_replay_client.cpp
    src/app/atproto/session.cpp
    src/app/atproto/writer.cpp
    src/app/attestation/signer.cpp
    src/app/attestation/proof.cpp
    src/app/audit/command.cpp
    src/app/audit/transport.cpp
    src/app/journal/command.cpp
    src/app/journal/rules.cpp
    src/app/control/state.cpp
    src/app/daemon/backoff.cpp
    src/app/daemon/config.cpp
    src/app/daemon/loop.cpp
    src/app/daemon/signals.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/outbound/action.cpp
    src/app/outbound/actions.cpp
    src/app/outbound/audit.cpp
    src/app/outbound/budget.cpp
    src/app/outbound/config.cpp
    src/app/outbound/evaluate.cpp
    src/app/outbound/execute.cpp
    src/app/state/lock.cpp
    src/app/sync/engine.cpp
  src/app/sync/jetstream_backfill.cpp
  src/app/sync/jetstream_archive.cpp
    src/app/state/time.cpp
    src/app/sync/linkage.cpp
    src/app/sync/parallel.cpp
    src/app/journal/mac.cpp
    src/app/journal/store.cpp)
  target_include_directories(atperson PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  target_link_libraries(atperson PRIVATE atperson-cpp wolfram-cpp)
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

  add_executable(atperson-core-test tests/core/core.c)
  target_link_libraries(atperson-core-test PRIVATE atperson-core)
  add_test(NAME atperson-core COMMAND atperson-core-test)

  # Generic dense neural kernel (#74): internal topology/layout coverage keeps
  # non-legacy shapes testable before snapshot v6 exposes public construction.
  add_executable(atperson-neural-kernel-test tests/core/neural_kernel.c)
  target_link_libraries(atperson-neural-kernel-test PRIVATE atperson-core)
  target_include_directories(atperson-neural-kernel-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  add_test(NAME atperson-neural-kernel COMMAND atperson-neural-kernel-test)

  add_executable(atperson-memory-test tests/memory/memory.c)
  target_link_libraries(atperson-memory-test PRIVATE atperson-core)
  add_test(NAME atperson-memory COMMAND atperson-memory-test)

  add_executable(atperson-familiarity-test tests/familiarity/familiarity.c)
  target_link_libraries(atperson-familiarity-test PRIVATE atperson-core)
  add_test(NAME atperson-familiarity COMMAND atperson-familiarity-test)

  add_executable(atperson-valence-test tests/valence/valence.c)
  target_link_libraries(atperson-valence-test PRIVATE atperson-core)
  add_test(NAME atperson-valence COMMAND atperson-valence-test)

  add_executable(atperson-action-test tests/action/action.c)
  target_link_libraries(atperson-action-test PRIVATE atperson-core)
  add_test(NAME atperson-action COMMAND atperson-action-test)

  add_executable(atperson-action-decision-test tests/action/decision.c)
  target_link_libraries(atperson-action-decision-test PRIVATE atperson-core)
  add_test(NAME atperson-action-decision COMMAND atperson-action-decision-test)

  add_executable(atperson-context-test tests/context/context.c)
  target_link_libraries(atperson-context-test PRIVATE atperson-core)
  add_test(NAME atperson-context COMMAND atperson-context-test)

  # The ledger test binary is re-invoked as *separate OS processes* so the
  # cross-run dedup test exercises a genuinely restarted process. Phase 1
  # commits entries for several posts; phase 2 (which CTest runs afterwards)
  # re-opens the same files and asserts the unique (source id + digest) index
  # blocks every already-committed post.
  add_executable(atperson-ledger-test tests/ledger/ledger.c)
  target_link_libraries(atperson-ledger-test PRIVATE atperson-core)
  add_test(
    NAME atperson-ledger_dedup_phase1
    COMMAND atperson-ledger-test dedup-phase1
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-runs)
  add_test(
    NAME atperson-ledger_dedup_phase2
    COMMAND atperson-ledger-test dedup-phase2
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-runs)
  set_tests_properties(atperson-ledger_dedup_phase2
                       PROPERTIES DEPENDS atperson-ledger_dedup_phase1)
  add_test(
    NAME atperson-ledger_roundtrip
    COMMAND atperson-ledger-test roundtrip
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-roundtrip)
  add_test(
    NAME atperson-ledger_crash_safety
    COMMAND atperson-ledger-test crash-safety
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-crash)
  add_test(
    NAME atperson-ledger_payloads
    COMMAND atperson-ledger-test payloads
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-payloads)
  add_test(
    NAME atperson-ledger_payload_corruption
    COMMAND atperson-ledger-test payload-corruption
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-payload-corruption)
  add_test(
    NAME atperson-ledger_migration
    COMMAND atperson-ledger-test migration
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-migration)
  add_test(
    NAME atperson-ledger_migration_v2
    COMMAND atperson-ledger-test migration-v2
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-migration-v2)
  add_test(
    NAME atperson-ledger_context
    COMMAND atperson-ledger-test context
            ${CMAKE_CURRENT_BINARY_DIR}/ledger-context)

  # Deterministic replay/rebuild from the ledger; pure core, no network.
  add_executable(atperson-replay-test tests/replay/replay.c)
  target_link_libraries(atperson-replay-test PRIVATE atperson-core)
  add_test(
    NAME atperson-replay
    COMMAND atperson-replay-test replay ${CMAKE_CURRENT_BINARY_DIR}/replay)
  add_test(
    NAME atperson-replay_rebuild_safety
    COMMAND atperson-replay-test rebuild-safety
            ${CMAKE_CURRENT_BINARY_DIR}/replay-safety)

  # Source withdrawal/unlearning semantics; pure core, no network.
  add_executable(atperson-withdraw-test tests/withdraw/withdraw.c)
  target_link_libraries(atperson-withdraw-test PRIVATE atperson-core)
  add_test(
    NAME atperson-withdraw
    COMMAND atperson-withdraw-test withdraw ${CMAKE_CURRENT_BINARY_DIR}/withdraw)

  # Safe ledger compaction and checkpointing; pure core, no network.
  add_executable(atperson-compact-test tests/compact/compact.c)
  target_link_libraries(atperson-compact-test PRIVATE atperson-core)
  add_test(
    NAME atperson-compact
    COMMAND atperson-compact-test compact ${CMAKE_CURRENT_BINARY_DIR}/compact)
  add_test(
    NAME atperson-compact_rebuild_equivalence
    COMMAND atperson-compact-test rebuild-equivalence
            ${CMAKE_CURRENT_BINARY_DIR}/compact-rebuild)

  # Unicode tokenization contract; pure core, no network.
  add_executable(atperson-tokenize-test tests/tokenize/tokenize.c)
  target_link_libraries(atperson-tokenize-test PRIVATE atperson-core)
  target_include_directories(atperson-tokenize-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  add_test(
    NAME atperson-tokenize
    COMMAND atperson-tokenize-test tokenize)
  add_test(
    NAME atperson-tokenize_graph
    COMMAND atperson-tokenize-test tokenize-graph)

  # Graph growth bounds and pathological input; pure core, no network.
  add_executable(atperson-growth-test tests/growth/growth.c)
  target_link_libraries(atperson-growth-test PRIVATE atperson-core)
  add_test(
    NAME atperson-growth
    COMMAND atperson-growth-test growth)
  add_test(
    NAME atperson-growth_pathology
    COMMAND atperson-growth-test growth-pathology)

  # Capacity/continual-learning benchmark foundation (#67). The smoke mode
  # crosses runtime capacity selection into the C23 core and stays bounded for
  # normal CI; the full five-profile run is in the explicit bench suite.
  add_executable(atperson-capacity-benchmark tests/resource/capacity_bench.cpp)
  target_link_libraries(atperson-capacity-benchmark PRIVATE atperson-cpp)
  target_include_directories(
    atperson-capacity-benchmark
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/app
      ${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource)
  add_test(
    NAME atperson-capacity-smoke
    COMMAND atperson-capacity-benchmark smoke)
  add_test(
    NAME atperson-capacity-bench
    COMMAND atperson-capacity-benchmark bench)
  set_tests_properties(atperson-capacity-bench PROPERTIES LABELS "bench")

  # Scale benchmarks (issue #9). Explicit suite, excluded from default
  # runs via label; invoke with: ctest -L bench
  add_executable(atperson-bench tests/core/bench.c)
  target_link_libraries(atperson-bench PRIVATE atperson-core)
  add_test(
    NAME atperson-bench
    COMMAND atperson-bench bench)
  set_tests_properties(atperson-bench PROPERTIES LABELS "bench")

  add_executable(atperson-snapshot-test tests/snapshot/snapshot.c)
  target_link_libraries(atperson-snapshot-test PRIVATE atperson-core)
  add_test(
    NAME atperson-snapshot
    COMMAND atperson-snapshot-test)

  add_executable(atperson-v6-topology-test tests/snapshot/v6_topology.c)
  target_link_libraries(atperson-v6-topology-test PRIVATE atperson-core)
  target_include_directories(atperson-v6-topology-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  add_test(
    NAME atperson-v6-topology
    COMMAND atperson-v6-topology-test)

  add_executable(atperson-cpp-test tests/cpp/graph.cpp)
  target_link_libraries(atperson-cpp-test PRIVATE atperson-cpp)
  add_test(NAME atperson-cpp COMMAND atperson-cpp-test)

  # Read-only rendering of authoritative C23 plan/context/decision traces.
  add_executable(atperson-action-inspection-test tests/action/inspection.cpp)
  target_link_libraries(atperson-action-inspection-test PRIVATE atperson-cpp)
  target_include_directories(atperson-action-inspection-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-action-inspection COMMAND atperson-action-inspection-test)

  # Advisory audit: evidence/state marshalling, question schema, and response
  # parsing/rendering are offline-testable; only the transport is excluded
  # from the ATPERSON_BUILD_NETWORK=OFF configuration.
  add_executable(atperson-audit-test tests/audit/audit.cpp)
  target_link_libraries(atperson-audit-test PRIVATE atperson-cpp)
  target_include_directories(atperson-audit-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-audit COMMAND atperson-audit-test)

  # Dynamic host/container resource probing and deterministic budget policy.
  add_executable(atperson-resource-test tests/resource/resource.cpp)
  target_link_libraries(atperson-resource-test PRIVATE atperson-cpp)
  target_include_directories(atperson-resource-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-resource COMMAND atperson-resource-test)

  # Explicit neural expansion (#66): offline writer-lock, ledger-boundary,
  # deterministic-seed and atomic snapshot replacement coverage.
  add_executable(
    atperson-neural-expand-test
    tests/resource/neural_expand.cpp
    src/app/cli/neural.cpp
    src/app/state/lock.cpp)
  target_link_libraries(atperson-neural-expand-test PRIVATE atperson-cpp)
  target_include_directories(
    atperson-neural-expand-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-neural-expand COMMAND atperson-neural-expand-test)

  # Ingestion-state format and sync-engine restart semantics; offline
  # (fixture page fetchers, no live network).
  add_executable(
    atperson-sync-test
    tests/sync/sync.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/sync/engine.cpp
    src/app/state/time.cpp
    src/app/sync/linkage.cpp
    src/app/journal/store.cpp)
  target_link_libraries(atperson-sync-test PRIVATE atperson-cpp)
  target_include_directories(atperson-sync-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-sync COMMAND atperson-sync-test)

  # Action-event linkage (#27): replies and quotes associate with executed
  # outbound actions by stable at-URI, never by text. Parent/root/quote
  # linkage, first-field-wins, duplicate suppression across restarts and
  # the engine-integrated path. Offline.
  add_executable(
    atperson-linkage-test
    tests/sync/linkage.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/sync/engine.cpp
    src/app/state/time.cpp
    src/app/sync/linkage.cpp
    src/app/journal/store.cpp)
  target_compile_features(atperson-linkage-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-linkage-test PRIVATE atperson-cpp)
  target_include_directories(atperson-linkage-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-linkage COMMAND atperson-linkage-test)

  # Operator control state (#22): fail-closed defaults, write gate, approval
  # binding, atomic persistence round-trip. Offline.
  add_executable(atperson-control-test tests/control/control.cpp
                                        src/app/control/state.cpp)
  target_link_libraries(atperson-control-test PRIVATE atperson-cpp)
  target_include_directories(atperson-control-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-control COMMAND atperson-control-test)

  # Deterministic end-to-end lifecycle harness (issue #28): multi-run
  # scenarios over the real ledger/snapshot/state files with a scripted
  # feed fixture — restarts, crash recovery, withdrawal flows, planning
  # abstention, and cross-run determinism. Offline, no network.
  add_executable(
    atperson-e2e-test
    tests/sync/lifecycle.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/sync/engine.cpp
    src/app/state/time.cpp
    src/app/sync/linkage.cpp
    src/app/journal/store.cpp)
  target_link_libraries(atperson-e2e-test PRIVATE atperson-cpp)
  target_include_directories(atperson-e2e-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-e2e COMMAND atperson-e2e-test)

  # Long-running ingestion daemon (#21): scheduling, bounded per-cycle work,
  # retry backoff, snapshot cadence and graceful shutdown, driven through
  # offline hooks over the real sync engine. No network.
  add_executable(
    atperson-daemon-test
    tests/daemon/daemon.cpp
    src/app/daemon/backoff.cpp
    src/app/daemon/config.cpp
    src/app/daemon/loop.cpp
    src/app/ingestion/policy.cpp
    src/app/ingestion/state.cpp
    src/app/sync/engine.cpp
    src/app/state/time.cpp
    src/app/sync/linkage.cpp
    src/app/journal/store.cpp)
  target_compile_features(atperson-daemon-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-daemon-test PRIVATE atperson-cpp)
  target_include_directories(atperson-daemon-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-daemon COMMAND atperson-daemon-test)

  # Worker pool (#29 parallel sync): fail-closed task propagation,
  # backpressure and shutdown. Offline, no network.
  add_executable(
    atperson-worker-test
    tests/worker/pool.cpp
    src/app/worker/pool.cpp)
  target_compile_features(atperson-worker-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-worker-test PRIVATE atperson-cpp)
  target_include_directories(atperson-worker-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-worker COMMAND atperson-worker-test)

  # Journal-integrity MAC (#57, scoped): known-answer SHA-256 and HMAC-SHA256
  # vectors, create/verify round-trip and negative cases. Offline, no network.
  add_executable(
    atperson-journal-mac-test
    tests/journal/mac.cpp
    src/app/journal/mac.cpp)
  target_compile_features(atperson-journal-mac-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-journal-mac-test PRIVATE atperson-cpp)
  target_include_directories(atperson-journal-mac-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-journal-mac COMMAND atperson-journal-mac-test)

  # Outbound action policy and rate budgets (#23): default-deny, per-window
  # limits, cooldowns, duplicate suppression, restart persistence and the
  # inspectable CLI surface. Offline, no network.
  add_executable(
    atperson-outbound-test
    tests/outbound/outbound.cpp
    src/app/cli/outbound.cpp
    src/app/control/state.cpp
    src/app/outbound/actions.cpp
    src/app/outbound/budget.cpp
    src/app/outbound/config.cpp
    src/app/outbound/evaluate.cpp)
  target_compile_features(atperson-outbound-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-outbound-test PRIVATE atperson-cpp)
  target_include_directories(atperson-outbound-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-outbound COMMAND atperson-outbound-test)

  # Outbound execution (#25): the ordered gate chain (pause, policy, dry-run,
  # #22 approval), fail-closed refusal, budget recorded only on confirmed
  # success, idempotent frozen rkey, reply CID resolution and the audit log.
  # Offline: the network is a fake OutboundWriter, no Wolfram.
  add_executable(
    atperson-publish-test
    tests/outbound/execute.cpp
    src/app/control/state.cpp
    src/app/journal/store.cpp
    src/app/outbound/action.cpp
    src/app/outbound/actions.cpp
    src/app/outbound/audit.cpp
    src/app/outbound/budget.cpp
    src/app/outbound/config.cpp
    src/app/outbound/evaluate.cpp
    src/app/outbound/execute.cpp)
  target_compile_features(atperson-publish-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-publish-test PRIVATE atperson-cpp)
  target_include_directories(atperson-publish-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-publish COMMAND atperson-publish-test)

  # Action/outcome journal (#27): durable append-only record of the entity's
  # own outbound attempts, public events that reference executed records and
  # explicit valence applications. Round-trip, torn-tail recovery, linkage
  # lookup and dedup. Offline, no network.
  add_executable(
    atperson-journal-test
    tests/journal/store.cpp
    src/app/journal/store.cpp)
  target_compile_features(atperson-journal-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-journal-test PRIVATE atperson-cpp)
  target_include_directories(atperson-journal-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-journal COMMAND atperson-journal-test)

  # Outcome-to-valence mapping (#56): rule-table parsing, first-match-wins
  # ordering, within-seconds windows, idempotency across restarts, the
  # denied-outcome negative contract, and unknown-token skips through the
  # real command surface. Offline, no network.
  add_executable(
    atperson-journal-rules-test
    tests/journal/rules.cpp
    src/app/journal/command.cpp
    src/app/journal/rules.cpp
    src/app/journal/store.cpp
    src/app/state/lock.cpp
    src/app/state/time.cpp)
  target_compile_features(atperson-journal-rules-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-journal-rules-test PRIVATE atperson-cpp cjson)
  target_include_directories(atperson-journal-rules-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-journal-rules COMMAND atperson-journal-rules-test)

  # Property-based persistence round-trip invariants (issue #11): snapshot
  # save/load equivalence, ledger append/reopen equivalence, and replay
  # convergence, over reproducible pseudo-random inputs. Offline.
  add_executable(atperson-property-test tests/property/property.cpp)
  target_link_libraries(atperson-property-test PRIVATE atperson-cpp)
  add_test(NAME atperson-property COMMAND atperson-property-test)

  # Ingestion policy: every skip class produces the documented reason and
  # eligible classes are tagged (repost/reply). Offline, no network.
  add_executable(
    atperson-policy-test
    tests/policy/policy.cpp
    src/app/ingestion/policy.cpp)
  target_compile_features(atperson-policy-test PRIVATE cxx_std_23)
  target_include_directories(atperson-policy-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-policy COMMAND atperson-policy-test)

  # AT Protocol feed-item translation (issue #24): top-level posts, nested
  # replies, deleted parents, quotes, and malformed context, over real
  # feedViewPost JSON fixtures. Offline, no network.
  add_executable(
    atperson-extract-test
    tests/policy/extract.cpp
    src/app/atproto/extract.cpp
    src/app/ingestion/policy.cpp)
  target_compile_features(atperson-extract-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-extract-test PRIVATE atperson-cpp cjson)
  target_include_directories(atperson-extract-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-extract COMMAND atperson-extract-test)

  # Jetstream commit translation (#60): create/update/delete, other
  # collections, malformed envelopes, empty text, replies and quotes over
  # real commit JSON fixtures. Offline, no network.
  add_executable(
    atperson-jetstream-extract-test
    tests/atproto/jetstream.cpp
    src/app/atproto/jetstream.cpp
    src/app/atproto/extract.cpp
    src/app/ingestion/policy.cpp
    src/app/sync/engine.cpp
    src/app/state/time.cpp)
  target_compile_features(atperson-jetstream-extract-test PRIVATE cxx_std_23)
  target_link_libraries(atperson-jetstream-extract-test PRIVATE atperson-cpp cjson)
  target_include_directories(atperson-jetstream-extract-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-jetstream-extract COMMAND atperson-jetstream-extract-test)

  if(ATPERSON_BUILD_NETWORK)
    # Jetstream client construction can be tested without opening a socket:
    # persisted cursor/self/filter validation happens before lazy connect.
    add_executable(
      atperson-jetstream-client-test
      tests/atproto/jetstream_client.cpp
      src/app/atproto/jetstream_client.cpp
      src/app/atproto/jetstream.cpp
      src/app/ingestion/policy.cpp)
    target_compile_features(atperson-jetstream-client-test PRIVATE cxx_std_23)
    target_link_libraries(atperson-jetstream-client-test PRIVATE atperson-cpp wolfram-cpp cjson)
    target_include_directories(
      atperson-jetstream-client-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
    add_test(NAME atperson-jetstream-client COMMAND atperson-jetstream-client-test)

    add_executable(
      atperson-jetstream-replay-test
      tests/atproto/jetstream_replay.cpp
      src/app/atproto/jetstream_replay.cpp
      src/app/atproto/jetstream.cpp
      src/app/ingestion/policy.cpp
      src/app/sync/engine.cpp
      src/app/state/time.cpp)
    target_compile_features(atperson-jetstream-replay-test PRIVATE cxx_std_23)
    target_link_libraries(atperson-jetstream-replay-test PRIVATE atperson-cpp wolfram-cpp cjson)
    target_include_directories(atperson-jetstream-replay-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
    add_test(NAME atperson-jetstream-replay COMMAND atperson-jetstream-replay-test)

    add_executable(
      atperson-jetstream-archive-test
      tests/sync/jetstream_archive.cpp
      src/app/sync/jetstream_archive.cpp
      src/app/sync/engine.cpp
      src/app/ingestion/policy.cpp
      src/app/ingestion/state.cpp
      src/app/state/time.cpp)
    target_compile_features(atperson-jetstream-archive-test PRIVATE cxx_std_23)
    target_link_libraries(atperson-jetstream-archive-test PRIVATE atperson-cpp wolfram-cpp cjson)
    target_include_directories(
      atperson-jetstream-archive-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
    add_test(NAME atperson-jetstream-archive COMMAND atperson-jetstream-archive-test)
  endif()

  if(ATPERSON_BUILD_NETWORK)
    add_executable(
      atperson-attestation-signer-test
      tests/attestation/signer.cpp
      src/app/attestation/signer.cpp)
    target_compile_features(atperson-attestation-signer-test PRIVATE cxx_std_23)
    target_link_libraries(atperson-attestation-signer-test PRIVATE wolfram)
    target_include_directories(
      atperson-attestation-signer-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
    add_test(NAME atperson-attestation-signer
             COMMAND atperson-attestation-signer-test)

    add_executable(
      atperson-attestation-proof-test
      tests/attestation/proof.cpp
      src/app/attestation/proof.cpp
      src/app/attestation/signer.cpp)
    target_compile_features(atperson-attestation-proof-test PRIVATE cxx_std_23)
    target_link_libraries(atperson-attestation-proof-test PRIVATE wolfram)
    target_include_directories(
      atperson-attestation-proof-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
    add_test(NAME atperson-attestation-proof
             COMMAND atperson-attestation-proof-test)
  endif()

  add_executable(
    atperson-lock-test
    tests/sync/lock.cpp
    src/app/state/lock.cpp)
  target_compile_features(atperson-lock-test PRIVATE cxx_std_23)
  target_include_directories(atperson-lock-test PRIVATE ${ATPERSON_APP_PRIVATE_INCLUDE_DIRS})
  add_test(NAME atperson-lock COMMAND atperson-lock-test)

  add_executable(atperson-bootstrap-test tests/bootstrap/bootstrap.c)
  target_link_libraries(atperson-bootstrap-test PRIVATE atperson-core)
  if(UNIX)
    target_compile_definitions(atperson-bootstrap-test PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  add_test(NAME atperson-bootstrap COMMAND atperson-bootstrap-test)
endif()

# Fuzz targets (issue #11): libFuzzer harnesses over the untrusted-input
# boundaries — snapshot decoding, ledger open/recovery, tokenization.
# Clang-only (libFuzzer); never built by default or in ordinary CI test
# runs. Run manually with:
#   cmake -S . -B build-fuzz -DATPERSON_BUILD_NETWORK=OFF -DATPERSON_BUILD_FUZZ=ON
#   cmake --build build-fuzz
#   ./build-fuzz/snapshot-load-fuzzer -max_total_time=60 fuzz/corpus/snapshot
option(ATPERSON_BUILD_FUZZ "Build libFuzzer targets (requires Clang)" OFF)
if(ATPERSON_BUILD_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ATPERSON_BUILD_FUZZ requires Clang (libFuzzer)")
  endif()

  add_executable(snapshot-load-fuzzer fuzz/snapshot/load.c)
  target_link_libraries(snapshot-load-fuzzer PRIVATE atperson-core)
  target_include_directories(snapshot-load-fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

  # Regenerate committed v6 corpus seeds after a format change:
  #   cmake --build build-fuzz && ./build-fuzz/snapshot-v6-seeds fuzz/corpus/snapshot
  add_executable(snapshot-v6-seeds EXCLUDE_FROM_ALL fuzz/corpus/gen/snapshot_v6_seeds.c)
  target_link_libraries(snapshot-v6-seeds PRIVATE atperson-core)
  if(UNIX)
    target_compile_definitions(snapshot-load-fuzzer PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  target_compile_options(
    snapshot-load-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(snapshot-load-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)

  add_executable(ledger-open-fuzzer fuzz/ledger/open.c)
  target_link_libraries(ledger-open-fuzzer PRIVATE atperson-core)
  target_include_directories(ledger-open-fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  if(UNIX)
    target_compile_definitions(ledger-open-fuzzer PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  target_compile_options(
    ledger-open-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(ledger-open-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)

  add_executable(tokenize-fuzzer fuzz/tokenize/tokenize.c)
  target_link_libraries(tokenize-fuzzer PRIVATE atperson-core)
  target_include_directories(tokenize-fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  target_compile_options(
    tokenize-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(tokenize-fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
