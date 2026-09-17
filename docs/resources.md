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

Commands that do not need the current learned graph — including `rebuild`, `compact`, `withdraw` and cursor management — avoid loading it unnecessarily. Rebuild starts from a fresh graph rather than holding the old model in memory beside its replacement.

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

An override that exceeds available safe headroom is rejected. Explicit node/edge ceilings below the graph's current counts are raised to the current counts instead of evicting learned state.