---
title: "atperson: split graph.c into single-concern modules"
description: "graph.c (1223 lines, five concerns mashed together) is split into index, store, observe, and query translation units, all under the 500-line modular ceiling."
date: 2026-09-16
tags: [atperson, c, release]
draft: false
---

atperson `d232d3d` — the second split under issue #42, closing out the `graph.c` refactor.

## Before / after

`src/core/graph.c` was the largest core file at 1223 lines, mixing hash indexes, node interning, edge observation, observe walks, budget-ceiling checks, episodic-memory capture, recall, query, association ranking, and lifecycle helpers. It is now five files, each owning one concern and all comfortably under the 500-line ceiling from the modular-file mandate:

| Module | Lines | Concern |
|---|---|---|
| `graph.c` | 169 | construction, lifecycle, RNG, config, stats, status strings, ledger-entry accessors |
| `graph_index.c` | 196 | open-addressing node/edge hash indexes, `atp_find_node`, `atp_find_edge` |
| `graph_store.c` | 238 | node interning, edge observation, array growth (`atp_reserve_*`) |
| `graph_observe.c` | 321 | observe walks, budget-ceiling dry-run, episodic-memory capture |
| `graph_query.c` | 316 | query tokenization, recall, association ranking, familiarity lookup |

## Discipline enforced

- **Verbatim extraction.** No logic, signature, comment, or behaviour change — the split is purely organizational. The budget check that rejects whole observations before any mutation, the eviction tie-breaks, the splitmix64 edge-key finalizer, and every walk struct land in the module their concern owns.
- **Internal interface.** `internal.h` was the only header changed, and only to raise four index helpers from `static` to cross-TU visibility (`atp_node_index_maybe_grow`, `atp_node_index_insert`, `atp_edge_index_maybe_grow`, `atp_edge_index_insert`) plus their existing prototypes. TU-local structs (`atp_observe_walk`, `atp_budget_walk`, `atp_memory_walk`, `atp_query_walk`, `atp_recall_match`, `atp_ranked_edge`, `atp_lookup_walk`, `atp_token_count`) stay file-private.
- **Banner comments travel.** The `/* -------- hash indexes (issue #9) -------- */` and `/* -------- episodic memory -------- */` section markers attach to `graph_index.c` and `graph_observe.c` respectively.
- **Build/test green.** Core (GCC + Clang), ASan/UBSan, and network builds all pass — 31/31 tests on the merged main (including the concurrent action-termination inspection-traces work, integrated via a clean rebase onto upstream).

## Next

`src/core/persistence.c` (1037 lines) is the last file over the ceiling — its encode/decode/migration sections split next.
