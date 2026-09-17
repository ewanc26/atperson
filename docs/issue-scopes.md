# Issue scopes

Issue scopes name the subsystem that owns the authoritative change, not every file a patch happens to touch. Pick the narrowest scope that describes where behaviour actually changes. Use more than one only when the issue is specifically about a boundary between subsystems.

| Scope | Use for |
| --- | --- |
| `core/graph` | Tokenisation, graph topology, nodes/edges, association lookup, graph statistics and graph-facing C APIs |
| `core/learning` | Embeddings, neural scoring/training, sampling, learning rates and deterministic PRNG use |
| `core/memory` | Episodic retention, recall, consolidation, eviction, source linkage and summaries |
| `core/state` | Familiarity, valence and other inspectable learned state that is not graph topology or episodic memory |
| `core/action` | Candidate scoring, planning, sequence composition and learned decision evidence before network policy |
| `persistence` | Ledger, provenance, deduplication, snapshots, crash recovery, replay, migration, rebuild and withdrawal/unlearning |
| `runtime` | C++23 wrappers, lifecycle, configuration, filesystem/runtime orchestration, CLI, scheduling and process management |
| `network` | Wolfram-backed AT Protocol integration, feed/record translation, ingestion policy and explicit outbound network policy/execution |
| `build` | CMake, compilers, dependency pins, CI, sanitizers, packaging and repository tooling |
| `docs` | README, architecture/reference docs, examples and contributor/process documentation |
| `cross-cutting` | Work whose purpose genuinely changes multiple ownership boundaries |

## Boundary rules

The taxonomy follows the architecture rather than the programming language alone:

- authoritative learned state and learning algorithms live in C23;
- application/runtime orchestration lives in C++23;
- AT Protocol mechanics belong in Wolfram instead of being duplicated locally;
- serialised-format changes need an explicit compatibility/version decision;
- learned behaviour should remain inspectable through state, scores, counters, provenance or traces;
- network writes must pass explicit runtime policy/control rather than appearing as a side effect of learning code.

When work touches several areas, scope it to the subsystem where the authoritative behaviour changes and describe secondary effects in the issue body. Use `cross-cutting` when changing the boundary is itself the point.