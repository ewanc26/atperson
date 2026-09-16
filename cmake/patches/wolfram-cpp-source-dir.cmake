# Patch applied via FetchContent PATCH_COMMAND to the pinned Wolfram checkout.
#
# wolfram-src/cpp/CMakeLists.txt hard-codes ${CMAKE_SOURCE_DIR} when locating
# Wolfram's public C headers for the generated_owners.hpp RAII wrapper:
#
#   file(GLOB ... "${CMAKE_SOURCE_DIR}/include/wolfram/*.h")
#   COMMAND gen_owners_tool ${CMAKE_SOURCE_DIR}/include/wolfram ...
#
# When Wolfram is embedded via FetchContent, CMAKE_SOURCE_DIR is the top-level
# project (atperson), not Wolfram, so the codegen step scans a non-existent
# include/wolfram and aborts the build. This script rewrites those references
# to the actual Wolfram source directory.
#
# Required arguments:
#   WOLFRAM_SRC_DIR   absolute path to the populated wolfram source (e.g.
#                     ${FETCHCONTENT_BASE_DIR}/wolfram-src)
#   WOLFRAM_CPP_CMAKE absolute path to wolfram-src/cpp/CMakeLists.txt
#
# Idempotent: skips if the rewrite is already present.

if(NOT DEFINED WOLFRAM_SRC_DIR OR NOT DEFINED WOLFRAM_CPP_CMAKE)
  message(FATAL_ERROR "wolfram-cpp-source-dir patch: WOLFRAM_SRC_DIR and WOLFRAM_CPP_CMAKE are required")
endif()

file(READ "${WOLFRAM_CPP_CMAKE}" content)

string(FIND "${content}" "${WOLFRAM_SRC_DIR}/include" already_applied)
if(NOT already_applied EQUAL -1)
  message(STATUS "wolfram-cpp source-dir patch: already applied, skipping")
  return()
endif()

string(REPLACE "\${CMAKE_SOURCE_DIR}" "${WOLFRAM_SRC_DIR}" patched "${content}")

if(NOT patched STREQUAL content)
  file(WRITE "${WOLFRAM_CPP_CMAKE}" "${patched}")
  message(STATUS "wolfram-cpp source-dir patch: rewrote CMAKE_SOURCE_DIR to ${WOLFRAM_SRC_DIR}")
else()
  message(STATUS "wolfram-cpp source-dir patch: no CMAKE_SOURCE_DIR references to rewrite")
endif()