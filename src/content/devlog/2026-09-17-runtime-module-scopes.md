---
title: "atperson: move C++ runtime modules into scoped directories"
description: "The flat C++23 runtime implementation units and private headers now live under explicit action, ATProto, ingestion, resource, state, and sync scopes."
date: 2026-09-17
tags: [atperson, cpp, release]
draft: false
---

atperson `6b8c624` and `b399c1d` continue issue #42 by moving the remaining flat compound C++23 runtime modules into scope directories.

## What moved

The migration is deliberately structural. `6b8c624` moves the implementation blobs without changing them; `b399c1d` follows by moving the corresponding private headers and centralising the private scope include directories in CMake.

| Previous path | Scoped path |
|---|---|
| `src/app/action_inspection.cpp` | `src/app/action/inspection.cpp` |
| `src/app/action_inspection.hpp` | `src/app/action/action_inspection.hpp` |
| `src/app/atproto_client.cpp` | `src/app/atproto/client.cpp` |
| `src/app/atproto_client.hpp` | `src/app/atproto/atproto_client.hpp` |
| `src/app/ingestion_policy.cpp` | `src/app/ingestion/policy.cpp` |
| `src/app/ingestion_policy.hpp` | `src/app/ingestion/ingestion_policy.hpp` |
| `src/app/ingestion_state.cpp` | `src/app/ingestion/state.cpp` |
| `src/app/ingestion_state.hpp` | `src/app/ingestion/ingestion_state.hpp` |
| `src/app/resource_budget.cpp` | `src/app/resource/budget.cpp` |
| `src/app/resource_budget.hpp` | `src/app/resource/resource_budget.hpp` |
| `src/app/resource_runtime.cpp` | `src/app/resource/runtime.cpp` |
| `src/app/resource_runtime.hpp` | `src/app/resource/resource_runtime.hpp` |
| `src/app/system_resources.cpp` | `src/app/resource/system.cpp` |
| `src/app/system_resources.hpp` | `src/app/resource/system_resources.hpp` |
| `src/app/system_filesystems.cpp` | `src/app/resource/filesystems.cpp` |
| `src/app/state_lock.cpp` | `src/app/state/lock.cpp` |
| `src/app/state_lock.hpp` | `src/app/state/state_lock.hpp` |
| `src/app/sync_engine.cpp` | `src/app/sync/engine.cpp` |
| `src/app/sync_engine.hpp` | `src/app/sync/sync_engine.hpp` |

This makes ownership visible in the path instead of encoding both the scope and responsibility into one flat filename. In particular, the resource-management pieces now sit together while remaining separate atoms for host probing, filesystem probing, budget derivation, and runtime enforcement.

## Behaviour and authority

No learning, persistence, ingestion, resource-policy, or network behaviour changes in this migration. The C23 core remains authoritative for learned state and deterministic learning/replay; these C++23 modules remain runtime, integration, and presentation concerns.

The first implementation-only commit briefly exposed why the private headers needed to move in the same scope migration: the network target could no longer resolve headers such as `atproto_client.hpp` from the newly nested source directories. `b399c1d` fixes that structural dependency by putting the headers beside their scopes and making the private include-path contract explicit in CMake.

## Verification

The scoped-header tree builds and tests successfully on the core GCC, Clang, macOS and ASan/UBSan CI jobs. The Wolfram-backed network build is the remaining verification gate for the full runtime layout.

The next structural target is `src/app/main.cpp`, which still owns environment/configuration helpers, durable-path setup, command dispatch, resource checks, network-session setup and process control in one translation unit. That should be thinned without moving learned-state authority out of C23.
