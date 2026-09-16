# CI matrix

atperson's CI deliberately separates portable core coverage from the Wolfram-backed network build.

| Job | Platform/compiler | Network | Purpose |
| --- | --- | --- | --- |
| Core (Linux GCC) | Ubuntu / GCC | Off | Primary strict C23/C++23 build and unit tests |
| Core (Linux Clang) | Ubuntu / Clang | Off | Compiler-portability coverage |
| Core (macOS Clang) | macOS / Apple Clang | Off | Unix/macOS portability and ledger filesystem coverage |
| Core sanitizers | Ubuntu / Clang + ASan/UBSan | Off | Memory-safety and undefined-behaviour checks |
| Network | Ubuntu / default GCC | On | Full Wolfram-backed runtime build and test suite |

All core matrix jobs configure with `ATPERSON_BUILD_NETWORK=OFF`, so Wolfram or system networking dependencies cannot hide failures in the authoritative C23 learning core or the C++ wrapper layer.

The project requires strict C23 and C++23 modes (`CMAKE_C_EXTENSIONS=OFF` and `CMAKE_CXX_EXTENSIONS=OFF`). Platform-specific warning suppressions should not be added merely to make the matrix green; any required suppression should be narrow and documented next to the affected target or source.

The sanitizer job uses AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers enabled. Failures in either sanitizer are test failures, not advisory annotations.
