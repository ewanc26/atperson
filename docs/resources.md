# Dynamic resource budgeting

`atperson` adapts the amount of work it is willing to do to the machine or container it is actually running on. That policy is runtime-only: hardware size never becomes learned state and is never persisted as personality or preference.

The goals are straightforward: leave safety headroom, scale down cleanly on constrained systems, use more capacity when it is genuinely available, and refuse new work rather than partially learning an observation under pressure.

## What is measured

The C++23 runtime probes:

- effective CPU capacity, including fractional quotas;
- total and currently available memory;
- free space on every filesystem that can receive durable `atperson` state.

Linux cgroup v1/v2 limits are folded in when they are tighter than the host. This includes memory limits/current use, CPU quotas and cpuset restrictions where available. macOS uses `hw.memsize` and Mach VM statistics; Windows uses `GlobalMemoryStatusEx`; filesystem capacity comes from `std::filesystem::space`.

The runtime checks the destinations behind `ATPERSON_HOME`, `ATPERSON_STATE`, `ATPERSON_LEDGER` and `ATPERSON_INGESTION_STATE`. If state spans several filesystems, the tightest usable headroom becomes the process-wide write constraint.

```sh
atperson resources
```

prints both the detected resources and the budget derived from them.

## Memory and graph growth

The runtime reserves host/container headroom before allocating a graph-growth budget. The remainder is divided into conservative node and edge ceilings and passed to the C23 capacity API.

Those ceilings are never allowed below the graph's current size. If available memory falls, existing learned state remains intact; only further structural growth is restricted.

A zero-growth condition is represented as a real finite ceiling. It is never translated into the C API's `0 = unlimited` convention.

Before loading an existing snapshot, the runtime also performs a conservative preflight based on the snapshot size and current memory headroom. If the load no longer fits safely, it is refused before the process drifts into an avoidable OOM failure.

Commands that do not need the current learned graph — including `rebuild`, `compact`, `withdraw` and cursor management — avoid loading it unnecessarily. Rebuild reads only the persisted topology descriptor from the old snapshot (a seek-based metadata probe, not a graph load), builds a fresh graph at exactly that topology, and replays the ledger into it. It never holds the old model in memory beside its replacement.

## Neural capacity and first-run topology

Issue #73 binds the first run of a brand-new model generation to the machine's hardware capacity recommendation.

- **First creation**: when no model snapshot exists, the runtime derives the current resource budget first, translates the `NeuralCapacityRecommendation` into a validated C23 `atp_neural_architecture`, and creates a fresh graph at exactly that topology. The first save persists the architecture as durable model-generation metadata.
- **Every later run**: the persisted topology is authoritative. The current recommendation is only an execution/headroom check and an expansion suggestion. A smaller or changed host never silently reshapes the model; `atperson resources` keeps *active*, *recommended* and *expansion available* separate.
- **Rebuild**: recovers the model-generation topology from the durable snapshot via the metadata probe, never from current host hardware.
- **Refusal, not shrinkage**: if the current machine cannot safely host the persisted architecture's learned-state footprint within the memory safety reserve, load is refused with an explicit error. Free memory or move the generation to a larger machine; `ATPERSON_NEURAL_*` overrides never reshape an existing generation.
- **Pre-v6 generations** carry no topology descriptor and keep their legacy architecture; `rebuild` and `load` report the legacy topology for them. Future migration to variable shapes is a separate decision, not a side effect of this change.

The memory footprint check is deliberately an estimate: it includes values plus plasticity-importance storage for shared parameters and per-node embeddings, training scratch, and conservative structural node/edge allowances. Graph-growth ceilings use the active persisted embedding width so a wide model is not budgeted as though it still used the legacy 16D vectors.

## Disk and sync budgets

Each durable filesystem keeps a dynamic free-space reserve. If a destination is already inside that reserve, mutating commands fail before starting new durable work.

Timeline page size and per-run observation limits also shrink with effective CPU, memory and disk headroom. On a sufficiently constrained container, a page can fall to one item.

Multi-page syncs re-probe before each page, so changing cgroup limits or growing disk pressure can affect a running process without requiring a restart.

One-shot allocations are bounded as well: `ingest-file` checks size before reading a file into memory, and inspection commands reject result limits above the current safe allowance.

## What hardware does not change

Resource detection never changes model meaning. In particular it does not alter:

- tokenisation or learning equations;
- familiarity or episodic-memory retention rules;
- planner scoring, beam behaviour or stop thresholds;
- replay ordering or schema compatibility;
- persona, preference, valence meaning or social state.

A bigger machine may admit more bounded work. It must not make the same learned state mean something different.

## Overrides

Automatic budgeting is the default. Empty values or `0` mean automatic where supported.

| Variable | Meaning |
| --- | --- |
| `ATPERSON_MEMORY_BUDGET_BYTES` | Explicit graph-growth memory budget after the safety reserve |
| `ATPERSON_DISK_RESERVE_BYTES` | Free space that must remain unused on each durable filesystem |
| `ATPERSON_NODE_CAPACITY` | Explicit graph node ceiling |
| `ATPERSON_EDGE_CAPACITY` | Explicit graph edge ceiling |
| `ATPERSON_SYNC_PAGE_SIZE` | Explicit timeline page size (`1`–`100`) |
| `ATPERSON_SYNC_MAX_OBSERVATIONS` | Explicit per-run observation budget |
| `ATPERSON_NEURAL_CAPACITY` | Forced capacity class used at first creation: `constrained`, `baseline`, `capable`, `large`, `expansive` or `auto` |
| `ATPERSON_NEURAL_EMBEDDING_DIM` | Explicit first-creation embedding width |
| `ATPERSON_NEURAL_HIDDEN_LAYERS` | Explicit first-creation hidden layer count (`1`–`3`) |
| `ATPERSON_NEURAL_HIDDEN_WIDTHS` | Explicit first-creation hidden widths, colon-separated (e.g. `256:128`) |

An override that exceeds available safe headroom is rejected. Explicit node/edge ceilings below the graph's current counts are raised to the current counts instead of evicting learned state.

The neural overrides influence **first creation, rebuild-without-a-model, and the read-only expansion proposal**. Explicit shape overrides keep every field they leave unset from the automatic class's shape; the combined effective shape is validated as a whole (embedding within the C-core width limit, no zero-width active layer, no nonzero width beyond the declared layer count, parameter count within the C-core bound). A forced capacity class cannot be combined with the explicit shape variables, and both stay inside the runtime policy bounds: the recommendation's width vector expresses at most three hidden layers, so overrides above three are rejected even though the C core itself allows four. Overrides never reshape an existing model merely by being present; an override that parks a fresh model beyond current memory headroom is refused at creation.

## Neural expansion

`atperson neural status` is read-only. It requires an existing persisted model generation, compares that active topology with the current recommendation, and prints migration version 1, both shapes, parameter counts, estimated learned-state footprints, additional memory cost and whether the proposed shape fits current safe memory headroom. It always ends with `mutation: none (inspection only)`.

Migration-v1 eligibility is coordinate-wise and monotonic:

- proposed embedding width must be at least the active width;
- proposed hidden-layer count must be at least the active count;
- every pre-existing hidden layer must stay the same width or widen;
- when hidden layers are appended, the new final hidden width must still retain every old scalar-output weight;
- at least one dimension or layer count must increase for expansion to be available;
- an otherwise larger recommendation that narrows any active coordinate is incompatible.

`atperson neural expand` is the explicit mutation path. It never runs automatically. The command takes the state-directory writer lock before loading the model, re-probes the current resource recommendation under that lock, and refuses incompatible or insufficient-memory proposals before migration.

The mutation is bound to the greatest durable observation-ledger id at the time the lock is held. Its migration seed is derived deterministically from durable generation facts and then persisted; rebuild never has to derive that seed again. The C23 migration allocates the replacement network, embeddings and migration-history array before its commit point, preserves every overlapping learned value and plasticity-importance coordinate bit-for-bit, initialises only newly-created coordinates, and leaves the graph's ordinary RNG state unchanged.

The CLI performs that mutation on a separately loaded candidate generation. The old `model.bin` is not modified during preflight or allocation. Saving uses the snapshot writer's temporary-file + atomic-rename path, so the active snapshot is replaced only after the complete expanded v7 image has encoded and synced successfully.

Migrated generations persist ordered history in snapshot v7. Rebuild begins at the first migration's source architecture and applies each recorded migration immediately after its durable ledger boundary. Withdrawn, pending and failed observations still retain their place in ledger chronology, so withdrawal/rebuild cannot shift a topology transition merely because an observation no longer contributes learning.

If the active topology already matches the recommendation, `neural expand` reports a no-op. A weaker recommendation is refused rather than shrinking or rewriting the generation.

