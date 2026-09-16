# Dynamic resource budgeting

atperson derives runtime resource limits from the machine it is actually
running on. The policy is runtime-only: it does not become learned state and it
is not persisted in model snapshots.

The goals are:

- scale down safely on constrained machines and containers;
- use more headroom on larger hosts without a source-code profile change;
- preserve deterministic learning and planning semantics;
- refuse or bound new work under pressure rather than evicting existing learned
  state or partially learning an observation.

## What is detected

The C++23 runtime probes:

- effective CPU capacity;
- total and currently available memory;
- capacity and available bytes on the filesystem containing the atperson data
  directory.

On Linux, cgroup v2 and v1 memory/CPU limits are folded into the effective
values when they are tighter than the host. `memory.max`/`memory.high`,
`memory.current`, `cpu.max`, legacy v1 quota files and cpuset restrictions are
considered where present. This prevents a container with a 1 GiB memory limit
from behaving as if it owned all RAM on the host.

macOS uses `hw.memsize` plus Mach VM statistics. Windows uses
`GlobalMemoryStatusEx`. Filesystem capacity uses `std::filesystem::space`, so
custom `ATPERSON_HOME` locations are budgeted against the filesystem that will
actually hold the state.

Use:

```sh
atperson resources
```

to see both the detected resources and the currently derived budget.

## Budget derivation

Memory keeps a host/container safety reserve first. atperson then permits graph
growth from only part of the remaining currently available memory. The growth
budget is split between future nodes and edges using conservative per-item
reservations that are intentionally above the measured issue #9 benchmark
footprints.

The resulting node/edge values are passed directly to #9's
`atp_graph_set_capacity`. They are always at least the graph's current counts.
If available memory falls, existing learned state remains untouched; only new
structural growth becomes more restricted. A zero-growth situation is never
translated to the C core's `0 = unlimited` capacity convention.

Disk keeps a dynamic free-space reserve and allows one run to consume only a
portion of the headroom above it. If the filesystem is inside the safety
reserve, durable mutating commands fail before doing new work. Sync page size
and the per-run observation budget also shrink with disk, memory and effective
CPU capacity.

Longer multi-page syncs re-probe before each page. This allows cgroup limits,
memory pressure and disk headroom to change while a process is running without
requiring a restart.

## What is deliberately not dynamic

Machine size does **not** change learning or decision semantics. In particular,
resource detection does not alter:

- tokenization or learning equations;
- familiarity or episodic-memory retention rules;
- planner scoring, beam semantics or stop thresholds;
- replay ordering or schema compatibility;
- any persona, preference or social state.

Those are model semantics. A faster machine may admit more bounded work, but it
must not make the same learned state mean something different.

## Overrides

Automatic budgeting is the default. Operators may override individual limits
with environment variables; empty values or `0` mean automatic:

- `ATPERSON_MEMORY_BUDGET_BYTES` — graph-growth memory budget after the safety
  reserve. Values larger than currently available headroom are rejected.
- `ATPERSON_DISK_RESERVE_BYTES` — free-space reserve that atperson must leave
  untouched.
- `ATPERSON_NODE_CAPACITY` — explicit graph node ceiling.
- `ATPERSON_EDGE_CAPACITY` — explicit graph edge ceiling.
- `ATPERSON_SYNC_PAGE_SIZE` — explicit timeline page size, 1–100. Without an
  override the page size is derived from effective CPU, memory and disk.
- `ATPERSON_SYNC_MAX_OBSERVATIONS` — explicit per-run sync observation budget.

Explicit node/edge ceilings can be lower than the graph's current size; the
runtime raises them to the current counts rather than evicting state. This
matches #9's fail-closed growth semantics.
