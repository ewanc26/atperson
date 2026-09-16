# Source module layout

This file refines the repository-wide `AGENTS.md` rules for everything under `src/`.

## Mandatory scoped/atomic paths

Implementation and private-module files MUST use the form:

```text
./<scope>/<atom>.{c,h,cpp,hpp}
```

The `./` is relative to the appropriate source root (`src/core` for C23 learned-state code and `src/app` for C++23 runtime code). A scope is a cohesive subsystem; an atom is one focused responsibility within it.

Examples:

```text
src/core/graph/lifecycle.c
src/core/graph/index.c
src/core/graph/store.c
src/core/graph/observe.c
src/core/graph/query.c
src/core/ledger/api.c
src/core/ledger/format.c
src/core/ledger/recover.c
src/core/persistence/encode.c
src/core/persistence/decode.c
src/app/resource/budget.cpp
src/app/resource/runtime.cpp
```

Do not create new flat compound modules such as `graph_query.c`, `ledger_format.c`, `resource_runtime.cpp`, or similar. Existing flat files are migration debt, not precedent. When a legacy file is touched for substantive work, move the affected concern into a scoped atom when practical.

Public installed headers are the exception: the public API remains under `include/atperson/`. Scope-private headers belong beside their implementation atoms inside the relevant `src/core/<scope>/` or `src/app/<scope>/` directory.

## Atomicity

Each atom owns one responsibility and should remain substantially below 500 lines. Split an atom before unrelated behaviour pushes it toward that size. Do not create generic `util.*`, `misc.*`, or `helpers.*` dumping grounds; shared code needs a deliberately named scope and a concrete contract.

A file move/split must not silently change behaviour. Pure modularisation preserves public API/ABI, learning equations, deterministic replay order, ledger/snapshot formats, network policy, and persistence semantics. Behavioural changes belong in separate commits/PRs with their own tests and compatibility decisions.

Every new private module needs a short leading contract comment explaining what it owns, its important collaborators, ownership/allocation expectations where relevant, and failure semantics.

## Build discipline

Build manifests must switch to scoped paths in the same migration. Do not leave two compiled copies of the same implementation. Both network-off and network-on configurations must remain buildable, and the normal GCC/Clang/macOS/sanitizer/network CI matrix remains authoritative.

The C23/C++23 boundary from the root `AGENTS.md` is unchanged by directory structure: moving code into a scope never transfers authority between layers.