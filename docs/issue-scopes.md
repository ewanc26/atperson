# Issue scopes

Issue scopes describe the **primary subsystem that owns the work**, not every file an implementation might touch. Use the narrowest scope that captures the authoritative change. Architecture proposals may select more than one scope when the point of the issue is the boundary itself.

| Scope | Use for |
| --- | --- |
| `core/graph` | Tokenisation, graph topology, nodes, edges, association lookup, graph statistics, and graph-facing public C APIs. |
| `core/learning` | Embeddings, neural scoring/training, online updates, sampling, learning rates, deterministic PRNG use, and other learning algorithms. |
| `core/memory` | Episodic memory selection, consolidation, recall, eviction, source linkage, and memory-derived summaries. |
| `core/state` | Familiarity and future inspectable internal-state signals that are learned from experience but are not graph topology or episodic memory. |
| `core/action` | Candidate scoring, planning, sequence composition, evidence traces, and future learned decision primitives before network policy. |
| `persistence` | Observation ledger, provenance, deduplication, snapshots, format versions, crash recovery, replay, rebuild, migration, and unlearning/removal semantics. |
| `runtime` | C++23 wrappers, lifecycle, configuration, filesystem/runtime orchestration, CLI behaviour, scheduling, and process management. |
| `network` | Wolfram-backed AT Protocol integration, record/feed translation, session use, ingestion policy, and any future explicit network-action policy. |
| `build` | CMake, compiler support, dependency pinning, CI, sanitizers, packaging, and repository tooling. |
| `docs` | README, architecture documentation, examples, contributor guidance, and issue/process documentation with no behavioural code change. |
| `cross-cutting` | Work whose purpose genuinely spans subsystem ownership and cannot be represented by one primary scope. Prefer a specific scope whenever possible. |

## Boundary rules

The scope taxonomy follows the project architecture rather than language alone:

- authoritative learned state and learning algorithms belong to C23;
- application/runtime concerns belong to C++23;
- AT Protocol mechanics belong to Wolfram and should not be duplicated locally;
- persistence changes require an explicit compatibility/version decision when serialized representation changes;
- learned behaviour should remain inspectable through explicit state, scores, counters, provenance, or traces;
- autonomous network writes must be introduced as explicit policy, never as an incidental side effect of learning code.

When an issue touches several areas, select the subsystem where the authoritative behaviour changes and mention secondary effects in the issue body. Use `cross-cutting` only when deciding or changing the boundary is itself the work.
