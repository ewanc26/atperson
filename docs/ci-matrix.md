# CI matrix

CI separates portable core coverage from the Wolfram-backed network build so a protocol dependency cannot hide a regression in the learning core.

| Job | Platform/compiler | Network | Purpose |
| --- | --- | --- | --- |
| Core (Linux GCC) | Ubuntu / GCC | Off | Primary strict C23/C++23 build and unit tests |
| Core (Linux Clang) | Ubuntu / Clang | Off | Compiler portability |
| Core (macOS Clang) | macOS / Apple Clang | Off | macOS/Unix and filesystem coverage |
| Core sanitizers | Ubuntu / Clang + ASan/UBSan | Off | Memory-safety and undefined-behaviour checks |
| Network | Ubuntu / GCC | On | Full Wolfram-backed runtime build and tests |

Core jobs configure with `ATPERSON_BUILD_NETWORK=OFF`. The repository requires strict C23 and C++23 (`CMAKE_C_EXTENSIONS=OFF`, `CMAKE_CXX_EXTENSIONS=OFF`).

Warnings should be fixed rather than broadly suppressed. Where a suppression is genuinely necessary, keep it narrow and document it beside the affected target or source.

The sanitizer build enables AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. Sanitizer findings are test failures, not advisory output.