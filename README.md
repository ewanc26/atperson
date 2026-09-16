# atperson

`atperson` is an experiment in building a persistent digital entity on the
[AT Protocol](https://atproto.com/) whose learned state begins empty and grows
through experience rather than from a hand-written persona or system prompt.

The project is intentionally split across **C23** and **C++23**:

- **C23 owns authoritative learned state**: vocabulary, the language graph,
  trainable embeddings, neural association scoring, online learning, episodic
  memory, familiarity, inspectable action scoring, the durable observation
  ledger, and snapshot persistence.
- **C++23 owns the application/runtime boundary**: RAII wrappers, lifecycle and
  configuration, command-line orchestration, record extraction, and AT Protocol
  integration.
- **[Wolfram](https://github.com/ewanc26/wolfram) owns AT Protocol mechanics**:
  sessions, XRPC, identity, repository operations, and Bluesky-facing client
  behaviour are dependencies rather than reimplemented here.

`atperson` does not claim sentience or personhood, and it does not currently
publish autonomously. The aim is to make learning, memory, state, and eventual
behaviour durable and inspectable so that development does not collapse into a
hidden LLM prompt pretending to be a persistent individual.

## Status

The project now has working implementations for the first five architectural
stages, with later stages intentionally incomplete:

| Stage | State | Current implementation |
| --- | --- | --- |
| Language graph | Implemented | Empty-start vocabulary, directed associations, trainable 16D embeddings, online neural scoring and negative sampling |
| Observation ledger | Implemented | Crash-safe append-only C23 log, durable commit marker, provenance, outcomes, cross-run `(source id + digest)` deduplication, and retained canonical payloads (format v2, replayable) |
| Memory | First pass implemented | Source-linked episodic memories, token summaries, recall counters, deterministic eviction, and graph-backed semantic association |
| Internal state | First pass implemented | Per-token familiarity learned purely from repeated exposure |
| Action model | First pass implemented | Deterministic, read-only continuation candidates with inspectable score components |
| Network behaviour | Not enabled | Wolfram-backed ingestion exists; autonomous likes, replies, follows, reposts, and posts do not |
| Ingestion policy | Implemented | Explicit public-data policy: skip classes with machine-readable reasons, self-observation excluded, skipped items ledgered as `SKIPPED` |
| Deterministic replay | Implemented | `atp_replay_ledger` + `atperson rebuild`: rebuild learned state from the ledger alone, explicit outcome semantics, atomic snapshot replacement, byte-identical rebuilds |
| Withdrawal/unlearning | Implemented | `ATP_LEDGER_OUTCOME_WITHDRAWN` + `atperson withdraw <id|source|author>`: append-only idempotent exclusion, rebuild produces the state that would have existed without the withdrawn source |
| Long-running runtime | Future work | `sync` is an explicitly-invoked bounded run with persistent catch-up state rather than a continuously operating agent |

The model begins with **zero words and zero relationships**. Neural parameters
have small deterministic random initial values so learning can start, but there
is no seeded vocabulary, biography, ideology, personality, or preference set.

## What exists now

The C23 core currently provides:

- an initially empty directed token graph;
- 16-dimensional trainable embeddings created only when a token is observed;
- a small neural scorer trained online from observed bigrams with negative
  sampling;
- learned edge strength, exposure counts, and hashed source provenance;
- a durable observation ledger whose append/commit ordering is designed for
  crash recovery and restart-safe deduplication;
- source-linked episodic memory with selective retention, weighted token
  summaries, recall accounting, and deterministic least-recalled eviction;
- per-token **familiarity**, an exponentially weighted exposure signal updated
  when that token is encountered again;
- a read-only **action candidate** API that ranks learned continuations from
  context without mutating the graph;
- versioned binary snapshots containing the complete mutable learning state,
  including the ledger mirror, episodic memory, and familiarity state;
- deterministic PRNG state and learning counters required for persistence and
  reproducibility.

The C++23 layer provides RAII wrappers for the graph and ledger, the CLI/runtime,
and the Wolfram-backed AT Protocol ingestion path. Timeline observations are
committed to the ledger before they are learned, preventing a restarted process
from silently training on the same committed post again.

## Architecture

```text
AT Protocol network
        |
        v
+---------------------------+
| C++23 runtime             |
| - Wolfram session         |
| - feed/record extraction  |
| - lifecycle/config        |
| - future network policy   |
+-------------+-------------+
              |
              | observations
              v
+---------------------------+
| C23 learning core         |
| - token graph             |
| - embeddings              |
| - neural training         |
| - observation ledger      |
| - episodic memory         |
| - familiarity state       |
| - action candidate scores |
| - persistence             |
+-------------+-------------+
              |
              v
       durable state
```

The architectural rule is simple: **C++ may orchestrate the entity, but it must
not become the hidden authority for what the entity has learned.** Learned
state and the evidence behind decisions stay in the C23 core.

See [`docs/architecture.md`](docs/architecture.md) for the invariants, scoring
formulae, persistence details, and staged growth path.

## Action model

Stage 5 currently stops deliberately short of generating or publishing text.
The C23 API in [`include/atperson/action.h`](include/atperson/action.h) exposes:

```c
atp_status atp_graph_action_candidates(
    const atp_graph *graph,
    const char *context,
    atp_action_candidate *out,
    size_t capacity,
    size_t *out_count);
```

Candidates come only from learned outgoing graph associations. Each result
includes the final score plus the evidence used to construct it:

- learned association score;
- familiarity score;
- observation-support score;
- summed supporting observations;
- number of distinct context nodes supporting the candidate.

Queries are deterministic and read-only. Unknown context does not seed new
vocabulary, and querying the action model does not train or mutate the graph.
The C++ wrapper and `atperson candidates` command expose the same evidence
without recomputing it outside the core; see
[`docs/action-inspection.md`](docs/action-inspection.md). This is intended to
become the evidence surface consumed by later sequence planning and network
policy rather than being replaced by opaque prompt logic.

## Persistence and memory

The current snapshot format is **v4**. Snapshots persist the mutable language
graph and neural state together with the mirrored observation ledger, episodic
memory, familiarity values, counters, and PRNG state.

The observation ledger is separate from the snapshot and is the authority for
which external observations have been committed. It records source identity,
author identity where available, observation time, content digest, schema
version, and outcome. Its in-memory deduplication index is rebuilt from the log
when opened.

Episodic memories remain linked to ledger IDs so remembered material retains a
route back to its source evidence. Source deletion and unlearning are solved
by withdrawal plus rebuild: withdrawn observations are durably excluded from
the next rebuild, so learned state is what the entity would have been without
them.

## Build

A normal build fetches the pinned Wolfram revision and builds the full runtime:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For core work without network dependencies:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

The build uses strict C23 and C++23. Unix builds explicitly request POSIX.1-2008
for the ledger durability APIs rather than relying on GNU language extensions.

The Wolfram dependency is currently pinned to commit
`9e63f76ab0b4f97f2cb5c62a5d0129b3d9023917`.

GitHub Actions exercises Linux GCC, Linux Clang, macOS Apple Clang, an
ASan+UBSan core build, and the full Wolfram-backed network build. See
[`docs/ci-matrix.md`](docs/ci-matrix.md) for the supported CI matrix and what
each job verifies.

## Runtime

All atperson data lives under `~/.ewanc26/atperson/` by default: the model
snapshot (`model.bin`), the observation ledger (`ledger.bin`), and the
ingestion cursor (`ingestion-state.json`). Override individual paths with
`ATPERSON_STATE`, `ATPERSON_LEDGER`, and `ATPERSON_INGESTION_STATE`, or the
whole directory with `ATPERSON_HOME`.

On first run atperson bootstraps the data directory (mode `0700`) and writes
a `.env` template (mode `0600`) documenting every environment variable it
reads — fill in `ATPERSON_IDENTIFIER` and `ATPERSON_APP_PASSWORD` there and
source it before running `sync`:

```sh
set -a; . ~/.ewanc26/atperson/.env; set +a
```

The bootstrap is idempotent and never touches an existing directory or
`.env`.

```sh
./build/atperson stats
./build/atperson ingest "hello world" local:first-observation
./build/atperson ingest-file ./notes.txt
./build/atperson assoc hello
./build/atperson candidates "hello world" 10
./build/atperson familiarity hello
./build/atperson recall "hello world" 5
```

To learn from the authenticated account's public home timeline:

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"
export ATPERSON_SERVICE="https://bsky.social" # optional

./build/atperson sync 5
```

Credentials are read from environment variables rather than command-line
arguments. Do not commit them.

`sync` consumes up to `max-pages` bounded timeline pages per run through
Wolfram's cursor-aware timeline API. Each page is processed durably before
its cursor is checkpointed, so a crash mid-page refetches the same page and
ledger deduplication suppresses anything already committed. An interrupted
traversal is resumed on the next run from the persisted cursor; once the
timeline is exhausted the cursor is cleared and the next sync starts at the
current head again. A cursor the service rejects is reported and discarded —
the ledger, not the cursor, is the authority for what has been learned.

Inspect or reset the catch-up position:

```sh
./build/atperson cursor status
./build/atperson cursor reset
```

### Rebuild

Reconstruct learned state from the observation ledger alone — no network
access, no snapshot required:

```sh
./build/atperson rebuild
```

Entries replay in ledger id order through the same observe path `sync`
uses, so the rebuilt graph is what the original run would have produced from
the same bytes. `LEARNED` entries re-train from their retained payloads;
`SKIPPED` entries mirror only; `PENDING`/`FAILED` are excluded. The new
snapshot replaces the old one atomically — a failure at any point leaves the
previous snapshot untouched, and two rebuilds of the same ledger produce
byte-identical snapshots. A payload-less `LEARNED` entry (v1-migrated
ledger) or an unimplemented schema version fails the rebuild rather than
silently producing a graph that never saw those bytes.

### Withdrawal

Durably exclude observations from future learned state:

```sh
./build/atperson withdraw id 42
./build/atperson withdraw source at://did:plc:example/app.bsky.feed.post/abc
./build/atperson withdraw author did:plc:example
```

Withdrawal is an append-only, idempotent ledger patch — log history is never
rewritten, and the patch sequence on disk is the audit trail. It never
mutates the live graph: run `atperson rebuild` to apply it, which excludes
withdrawn observations from graph, neural training, familiarity, memory,
counters, and the snapshot mirror. An edited record (same URI, new content)
appends a fresh entry, so withdrawing old content doesn't block the edit.

`sync` records each fetched post in the durable ledger before learning from it.
Already committed `(source id + digest)` pairs are skipped on later runs. Empty
records remain represented in the ledger but are not trained. Trainable posts
flow through the episodic-memory path and can be recalled by overlapping known
tokens.

### Ingestion policy

Every fetched record passes an explicit policy layer before it can become an
observation. Skipped classes, with their machine-readable reasons:

| Reason | Skipped when |
| --- | --- |
| `unsupported-record` | the record is not an `app.bsky.feed.post` |
| `self-authored` | the post is the authenticated account's own output |
| `viewer-blocked` / `viewer-blocked-by` / `viewer-muted` | the account blocks or muted the author, or the author blocks the account |
| `moderation-filtered` | a moderation decision filtered the post |
| `empty-text` | the post has no text |
| `non-text-only` | the post is images/video with no text |

Replies and reposts are learned and tagged with their reason. Self-observation
is an explicit exclusion, not an accident: learning from the account's own
output would create a feedback loop. Policy-skipped items are still recorded in
the ledger with outcome `SKIPPED`, so observed-but-not-learned stays
distinguishable from never-fetched, and timeline replays remain idempotent.

`sync` is still an explicitly-invoked, bounded run at this stage. A
continuously operating runtime should come only after replay/unlearning
semantics, explicit action policy, rate limiting, operator controls, and
stronger recovery behaviour are in place.

Mutating commands (`ingest`, `ingest-file`, `sync`, `cursor reset`) take an
exclusive writer lock on the data directory before touching durable state, so
two processes cannot mutate the same state concurrently. A lock left behind
by a dead process is detected and reclaimed automatically. Read-only commands
run without the lock and see state as of their own read — a concurrent writer
may commit after the reader started.

## Current boundaries

`atperson` intentionally does **not** currently:

- read private messages or private data;
- autonomously like, follow, reply, repost, or publish;
- use an LLM as a hidden personality or decision engine;
- seed a biography, ideology, preferences, or opinions into learned state;
- equate generated language with consciousness or personhood;
- pretend source deletion and learned-contribution removal are solved.

The next meaningful work is not simply "let it post". The action model needs
higher-level sequence planning and explicit network policy. Autonomous output
should sit on top of those observable mechanisms, not bypass them.

## Licence

AGPL-3.0. See [`LICENSE`](LICENSE).
