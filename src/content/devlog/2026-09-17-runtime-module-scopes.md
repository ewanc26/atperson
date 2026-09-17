---
title: "atperson: move C++ runtime implementations into scoped modules"
description: "The flat C++23 runtime implementation units now live under explicit action, ATProto, ingestion, resource, state, and sync scopes without changing their implementation bytes."
date: 2026-09-17
tags: [atperson, cpp, release]
draft: false
---

atperson `6b8c624` continues issue #42 by moving the remaining flat compound C++23 implementation units into scope directories.

## What moved

The migration is deliberately structural. The implementation blobs themselves are unchanged; only their repository paths and CMake source references moved.

| Previous path | Scoped path |
|---|---|
| `src/app/action_inspection.cpp` | `src/app/action/inspection.cpp` |
| `src/app/atproto_client.cpp` | `src/app/atproto/client.cpp` |
| `src/app/ingestion_policy.cpp` | `src/app/ingestion/policy.cpp` |
| `src/app/ingestion_state.cpp` | `src/app/ingestion/state.cpp` |
| `src/app/resource_budget.cpp` | `src/app/resource/budget.cpp` |
| `src/app/resource_runtime.cpp` | `src/app/resource/runtime.cpp` |
| `src/app/system_resources.cpp` | `src/app/resource/system.cpp` |
| `src/app/system_filesystems.cpp` | `src/app/resource/filesystems.cpp` |
| `src/app/state_lock.cpp` | `src/app/state/lock.cpp` |
| `src/app/sync_engine.cpp` | `src/app/sync/engine.cpp` |

This makes ownership visible in the path instead of encoding both the scope and responsibility into one flat filename. In particular, the resource-management pieces now sit together while remaining separate atoms for host probing, filesystem probing, budget derivation, and runtime enforcement.

## Behaviour and authority

No learning, persistence, ingestion, resource-policy, or network behaviour changes in this migration. The C23 core remains authoritative for learned state and deterministic learning/replay; these C++23 modules remain runtime, integration, and presentation concerns.

The private headers are still flat for the moment so this commit can remain a byte-for-byte implementation move. They are the next layout cleanup, followed by thinning the oversized `src/app/main.cpp` command/runtime composition unit.

## Verification

The existing CI matrix is running against the scoped paths. The migration updates both normal/network build manifests and the tests that compile ingestion, sync, and locking sources directly.
