# CI matrix

CI separates portable core coverage from the Wolfram-backed network build so a protocol dependency cannot hide a regression in the learning core.

| Job | Platform/compiler | Network | Purpose |
| --- | --- | --- | --- |
| Core (Linux GCC) | Ubuntu / GCC | Off | Primary strict C23/C++23 build and unit tests |
| Core (Linux Clang) | Ubuntu / Clang | Off | Compiler portability |
| Core (macOS Clang) | macOS / Apple Clang | Off | macOS/Unix and filesystem coverage |
| Core sanitizers | Ubuntu / Clang + ASan/UBSan | Off | Memory-safety and undefined-behaviour checks |
| Network | Ubuntu / GCC | On | Full Wolfram-backed runtime build and tests |
| Smoke fuzz | Ubuntu / Clang | Off | Thirty-second bounded run for each libFuzzer target |

Core jobs configure with `ATPERSON_BUILD_NETWORK=OFF`. The repository requires strict C23 and C++23 (`CMAKE_C_EXTENSIONS=OFF`, `CMAKE_CXX_EXTENSIONS=OFF`).

Build steps pass `--parallel 2` explicitly and the workflow also sets `CMAKE_BUILD_PARALLEL_LEVEL=2` and `MALLOC_ARENA_MAX=2`. The explicit count is what actually bounds peak compiler memory: `cmake --build <dir> --parallel` with no job count resolves to the native processor count and ignores `CMAKE_BUILD_PARALLEL_LEVEL`. The malloc-arena cap keeps glibc from giving each compiler thread its own arena on a multi-core runner, which inflates GCC's resident set. Neither setting reduces test coverage.

Test steps run with `ctest --timeout 300`. The non-bench tests are seconds-scale and filesystem/network-stub bound; a hang is a failure, and failing in five minutes beats sitting on a runner for the 25-minute default.

Warnings should be fixed rather than broadly suppressed. Where a suppression is genuinely necessary, keep it narrow and document it beside the affected target or source.

The sanitizer build enables AddressSanitizer and UndefinedBehaviorSanitizer with frame pointers. Sanitizer findings are test failures, not advisory output.