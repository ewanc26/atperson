# AGENTS.md

## Purpose and architectural invariant

`atperson` is an experimental persistent learning entity for the AT Protocol. It
must grow from observed experience rather than from a developer-authored
persona.

The project has a strict C23/C++23 boundary:

- **C23 owns authoritative learned state and learning/decision algorithms.**
  This includes the language graph, neural state, familiarity, episodic memory,
  semantic recall, author/source interaction state, action scoring and bounded
  planning primitives.
- **C++23 owns runtime and application concerns.** This includes configuration,
  filesystem/state-directory handling, process locking, scheduling, ingestion
  cursors, operator policy and orchestration.
- **`ewanc26/wolfram` owns AT Protocol mechanics.** Use Wolfram for sessions,
  XRPC/protocol operations and eventual repository writes. Do not copy or
  reimplement Wolfram protocol APIs inside atperson.

If a learning, memory or planning primitive cannot be built and tested with
`ATPERSON_BUILD_NETWORK=OFF`, the boundary is probably wrong.

## No seeded persona

The entity starts without a biography or developer-chosen personality.

Do not add hard-coded learned:

- opinions or ideology;
- political or religious preferences;
- biography or identity claims;
- favourite or disliked things;
- emotional history;
- friendships, trust, reputation or social hierarchy;
- vocabulary presented as prior experience;
- canned beliefs disguised as model state.

Small random neural initialisation is implementation state, not personality.
Tests and fixtures may use deterministic toy observations.

Prefer experience-derived, inspectable signals over labels that imply meaning
which the observations do not support. For example, repeated exposure may
produce familiarity; it must not silently become trust or friendship.

## Durable experience and reconstructability

The durable observation ledger is the authority for reconstructable experience.
Learned state must remain explainable and rebuildable from durable observations.

Preserve these invariants:

- deduplication is based on stable source identity plus content digest;
- committed observations are not trained twice after restart;
- replay is deterministic for a fixed compatible learning schema;
- source withdrawal is durable and takes effect in learned state after rebuild;
- compaction may remove dead representation details, but must preserve final
  ledger outcomes, stable entry identity and dedup semantics;
- episodic memories and derived author/source state must remain consistent with
  replay, snapshots, compaction and withdrawal;
- do not introduce a second mutable counter store when the value can be derived
  reliably from existing authoritative state.

Do not treat a snapshot as a substitute for the durable experience ledger.
Snapshots are portable persisted learning state and must remain versioned and
validated.

Snapshot-format changes require an explicit format-version/migration decision
and round-trip/corruption tests. Changes to learning semantics must also review
`ATPERSON_SCHEMA_VERSION` and replay compatibility. Read-only inspection or
ranking changes do not automatically require a learning-schema bump; changes
that alter what an observation learns do.

## Runtime ingestion state

Runtime ingestion cursor state is C++23 operational metadata, not learned C23
state. Keep it outside model snapshots.

The persistent cursor represents an interrupted catch-up traversal only. It is
opaque server state: do not parse it, infer time from it, compare cursors, or
use it as proof that an observation has been learned. Reuse a cursor only when
its recorded source identity matches the current service/account/endpoint
context. After catch-up exhaustion, clear it; a later independent sync begins
again at the timeline head and relies on the durable ledger for deduplication.

## Memory and internal state

Episodic recall must remain deterministic and inspectable.

The richer recall path may combine explicit components such as exact token
support, learned graph association, bounded familiarity, recency and previous
recall use. Keep those components separately inspectable. Do not replace this
with opaque embedding-only retrieval or hidden LLM summarisation.

Semantic recall may surface a zero-literal-overlap episode only when learned
graph evidence supports it. Familiarity, recency or previous use must not make
an otherwise unrelated memory eligible by themselves.

Unknown query/context tokens must not mutate vocabulary or learned state.

Author/source interaction state should use stable DIDs and AT URIs where
possible. Handles are mutable presentation metadata. Neutral exposure counters
and bounded familiarity are acceptable; trust, affinity, friendship,
reputation, sentiment or preference require explicit evidence and modelling.

## Planning and action model

Planning belongs in C23 and must remain bounded, deterministic and inspectable.

Preserve the current principles:

- hard limits on search depth/length, beam width, items and work;
- every candidate/plan step retains the evidence used to score it;
- deterministic tie-breaking for fixed learned state and inputs;
- planning/inspection is read-only unless an API explicitly documents a
  learning mutation;
- core planning performs no network I/O;
- cycles and dead ends terminate through explicit bounded semantics rather than
  accidental resource exhaustion.

Structured context selection for planning also belongs in C23. Do not hide
behaviour in a parallel C++ prompt/context heuristic. Selected memories,
interaction state and later preference signals should carry provenance and an
inspectable selection reason/score.

Fluent output is not sufficient reason to act. The action layer must eventually
be able to abstain and explain why no supported action was selected.

## Network and safety boundary

The current network path is for controlled public-data ingestion. Do not add
autonomous posts, replies, likes, follows, reposts, DMs or moderation actions
as a side effect of learning, memory or planning work.

Outbound behaviour must remain fail-closed until the roadmap's explicit
operator controls, outbound policy/rate budgets and Wolfram-backed write path
are implemented and tested. Do not bypass those gates to demonstrate that a
planner can produce text.

Do not ingest private messages into the learning graph.

Never log, commit or persist app passwords, tokens or other authentication
secrets. Keep credentials in runtime configuration only and minimise their
lifetime in memory.

AT Protocol policy belongs in the C++ runtime; learned scoring/state belongs in
C23; protocol mechanics belong in Wolfram.

## Modular, atomic files

Files must be modular and atomic. This is a hard requirement, not a
preference.

- One file owns one concern. A reader should be able to hold the file's
  purpose, its collaborators and its failure modes in mind at once.
- Keep source files small. When a file grows past roughly 500 lines,
  split it by concern before extending it further. `ledger.c` and
  `graph.c` predate this rule; new work must not add to the problem, and
  touching a large file for other reasons is a good moment to extract a
  coherent module.
- Every file must build and test independently of unrelated changes.
  A commit that touches a file must leave every configured build
  (`ATPERSON_BUILD_NETWORK=OFF` and `ON`) green.
- Extract, don't entangle: shared helpers get their own translation
  unit and a header comment stating the contract, rather than being
  copied or wedged into an unrelated file. The tokenizer duplication
  that produced five byte-scanner copies is the cautionary example.
- Headers document ownership: which functions a module exports, who
  allocates, who frees, and what the failure modes are.

## Concurrency

Multithreading is C++23 runtime territory unless the C23 core API
explicitly documents thread safety.

- The C23 core is single-threaded by contract. `atp_graph` and
  `atp_ledger` have no internal locking; callers own serialisation.
  Do not add locks inside core objects — instead expose a documented
  concurrency model when a real need arrives.
- The C++23 runtime may parallelise ingestion, I/O and orchestration,
  but every touch of a core object must go through one owner thread or
  an explicit serialisation point. Prefer message passing
  (queues, staged batches) over shared mutable state.
- Learning stays deterministic: parallel work must not change what an
  observation learns, the order of ledger commits, or replay results.
  If parallelising changes learned state, the design is wrong.
- Fail-closed applies to concurrency: on any error, leave core state
  untouched and report; never half-apply a batch because one worker
  failed.

## C23 core

Public C APIs live in `include/atperson/` and implementations in `src/core/`.

Do not introduce C++ types, exceptions, STL or Wolfram dependencies into the C
core. Keep APIs explicit about ownership, mutability, bounds and failure modes.

Prefer deterministic data structures and stable tie-breakers. Avoid hidden
global state, unbounded allocations driven directly by network input, and
silent clamping when a caller should instead receive an error.

When adding learned state, define persistence, replay, withdrawal and
inspection semantics at the same time rather than leaving them as future
cleanup.

## C++23 runtime

Keep wrappers such as `LanguageGraph` and `Ledger` thin. Do not move learning,
recall, scoring or planning policy into wrappers merely because C++ is more
convenient.

Use RAII for Wolfram, JSON and operating-system resources.

Configuration parsing, filesystem setup, state locking, scheduling, ingestion
state, recovery/backoff and network/operator policy belong here.

Runtime code may translate protocol observations into C-core inputs, but it
must not maintain a second hidden model of what the entity knows.

## Verification

For core-only work:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

For changes touching public C APIs, C++ integration, ingestion or networking,
also configure/build/test with `ATPERSON_BUILD_NETWORK=ON` so the Wolfram-backed
runtime remains compatible.

CI currently exercises Linux GCC, Linux Clang, macOS Clang, ASan/UBSan and the
full Linux Wolfram-backed network build. Treat failures in any relevant leg as
real failures; do not weaken CI to make a change pass.

Add focused regression tests for behaviour changes. Persistence work should
cover restart/round-trip behaviour; learned-state changes should cover replay;
withdrawal-sensitive state should cover rebuild after withdrawal; ranking and
planning work should cover ties, bounds and repeated deterministic calls.

## Repository discipline

This is an existing repository. Inspect the current code, README, roadmap,
issues and recent commits before assuming documentation is current.

Do not:

- run `git init` or replace repository history;
- overwrite unrelated user changes;
- force-push rewritten history without an explicit requirement;
- manufacture or backdate commit timestamps;
- create commits with timestamps in the future relative to the real current
  time;
- mix unrelated cleanup into a feature/fix commit.

Keep commits atomic, scoped and accurately described. Follow existing commit
prefixes where practical (`feat(core):`, `feat(memory):`, `feat(action):`,
`fix(...)`, `test:`, `docs:`, `build:`, `chore:`).

Use the normal PR/CI path for substantial changes. When work completes a
tracked issue, close it through the PR when appropriate and keep umbrella
roadmap checklists in sync.

Before finishing, verify the effective diff, repository status, tests and the
current `main` base. Do not claim verification that was not actually run.