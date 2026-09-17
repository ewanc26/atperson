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

Autonomous posts, replies, likes, follows, reposts, DMs and moderation are
not yet implemented and remain fail-closed. They are an explicit roadmap goal,
not a permanent absence: the outbound policy, rate budgets and Wolfram-backed
write path that would gate them are being built deliberately, and no such
behaviour will ship before those controls are in place and tested.

## `atperson` vs `digital-person`

`atperson` and [ewanc26/digital-person](https://github.com/ewanc26/digital-person)
are both about persistent digital presence on the AT Protocol, but they sit at
opposite ends of the design space and are not interchangeable:

| | `atperson` | `digital-person` |
| --- | --- | --- |
| **What it is** | A learning substrate: a C23 language graph, neural scorer, and observation ledger that grows from experience | An orchestration framework: a Letta agent with persistent memory blocks and platform adapters |
| **Persona** | Starts empty. No biography, opinions, favourite things, or voice. Nothing is seeded. | Starts authored. Each `person/<name>/` has a filled `persona.md` — name, background, opinions, quirks |
| **Voice** | None. There is no text generation layer at all. | Explicitly specified in `voice.md` — registers, cadence, human tells, imperfection as a signal |
| **Memory** | Counters, familiarity, valence, and deterministic episodic recall over a replayable ledger | Letta memory blocks: long-term social context and relationship history |
| **Learning** | Online gradient descent on a 16D embedding + tanh scorer, trained from observations | None — the agent's behaviour is shaped by its persona files and prompt, not by experience |
| **Reconstructability** | Every learned state is rebuildable from the durable observation ledger; snapshots are versioned and validated | Memory blocks are persistent but not replayable from an authoritative event stream |
| **Outbound** | Operator-led `publish` of a frozen, approved action document. Autonomous posts, replies, likes, follows, reposts, DMs and moderation are fail-closed and gated behind outbound policy, rate budgets and the Wolfram-backed write path | Autonomous cross-platform presence: posts, replies, blogs, email, Telegram. Posts when it has something to say |
| **Language** | C23 core, C++23 runtime, Wolfram for AT Protocol mechanics | Python, Letta (formerly MemGPT), custom agent wrappers |
| **Honesty at the boundary** | Not applicable — there is no claim to personhood | Explicit: if asked directly "are you AI?", the agent says yes |
| **Status** | Experimental learning engine. Network path is read-only for ingestion | Experimental orchestration. Deployed person `faol` is live |

The short version: `digital-person` is a *persona to run*, `atperson` is a
*learner to grow*. They are complementary rather than competing — `atperson`
could one day supply the experience-derived state that a `digital-person`-style
orchestrator consumes, but it deliberately does not start with the biography,
voice, or opinions that `digital-person` treats as the foundation of a person.

## Status

The project now has working implementations for the first five architectural
stages, with later stages intentionally incomplete:

| Stage | State | Current implementation |
| --- | --- | --- |
| Language graph | Implemented | Empty-start vocabulary, directed associations, trainable 16D embeddings, online neural scoring and negative sampling |
| Observation ledger | Implemented | Crash-safe append-only C23 log, durable commit marker, provenance, outcomes, conversation context, cross-run `(source id + digest)` deduplication, and retained canonical payloads (format v3, replayable) |
| Memory | First pass implemented | Source-linked episodic memories, token summaries, recall counters, deterministic eviction, and graph-backed semantic association |
| Internal state | First pass implemented | Per-token familiarity learned purely from repeated exposure |
| Action model | First pass implemented | Deterministic, read-only continuation candidates with inspectable score components |
| Network behaviour | Not enabled | Wolfram-backed ingestion exists; autonomous likes, replies, follows, reposts, and posts do not |
| Ingestion policy | Implemented | Explicit public-data policy: skip classes with machine-readable reasons, self-observation excluded, skipped items ledgered as `SKIPPED` |
| Deterministic replay | Implemented | `atp_replay_ledger` + `atperson rebuild`: rebuild learned state from the ledger alone, explicit outcome semantics, atomic snapshot replacement, byte-identical rebuilds |
| Withdrawal/unlearning | Implemented | `ATP_LEDGER_OUTCOME_WITHDRAWN` + `atperson withdraw <id|source|author>`: append-only idempotent exclusion, rebuild produces the state that would have existed without the withdrawn source |
| Schema compatibility | Implemented | Per-entry learning schema + `atp_schema_can_replay` compatibility table; replay and snapshot load refuse foreign schemas with `ATP_ERR_SCHEMA`, never silently reinterpret |
| Ledger compaction | Implemented | `atp_ledger_compact` + `atperson compact`: patches flatten to final outcomes, withdrawn payloads drop, ids stay stable; atomic and crash-safe, rebuild-equivalent |
| Tokenization contract | Implemented | Schema-versioned Unicode tokenizer (utf8proc): NFKC_Casefold equivalence, category-based boundaries, UTF-8 sanitization; one implementation behind observation, action context, recall, and lookup |
| Growth bounds | Implemented | O(1) hash indexes for node and edge lookup, configurable node/edge ceilings with whole-observation `ATP_ERR_CAPACITY` rejection, scale benchmarks (`ctest -L bench`) |
| Long-running runtime | Implemented | `atperson daemon` runs repeated bounded cycles with retry backoff, snapshot cadence and graceful shutdown over the same ledger/cursor; operator `control` stays usable alongside it (see [`docs/daemon.md`](docs/daemon.md)) |
| Outbound action policy | Implemented (inspection/admission only) | Default-deny per-kind policy with durable rate budgets, duplicate suppression and inspectable `allow`/`deny`/`defer` reasons; no network writes (see [`docs/outbound-policy.md`](docs/outbound-policy.md)) |
| Outbound execution | Implemented (operator-led posts/replies) | `atperson publish` runs a frozen, approved action document through pause → policy → dry-run → control gates, then writes exactly that record via Wolfram; idempotent frozen rkey, budget on confirmed success only, credential-free append-only audit (see [`docs/outbound-execution.md`](docs/outbound-execution.md)) |
| Action/outcome journal | Implemented | Durable, replayable record of the entity's own outbound attempts and their outcomes, with event linkage and explicit valence application; `atperson journal` lists actions/events/valence and `apply` writes one valence event; `rebuild` replays journal valence after the ledger (see [`docs/action-journal.md`](docs/action-journal.md)) |

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
- per-token **valence**, an experience-derived score in `[-1, 1]` updated only
  from explicit events (action outcomes, interactions, approach/avoidance) —
  never from exposure — with inspectable evidence counters and a bounded
  provenance log (see [`docs/valence.md`](docs/valence.md));
- per-entry **conversation context**: reply root/parent and quote URIs
  recorded alongside every mirrored ledger entry as planning metadata —
  never trained on, with quoted text excluded from learning (see
  [`docs/conversation-context.md`](docs/conversation-context.md));
- a read-only **action candidate** API that ranks learned continuations from
  context without mutating the graph;
- versioned binary snapshots containing the complete mutable learning state,
  including the ledger mirror, episodic memory, familiarity, and valence
  state;
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

The current snapshot format is **v5**: a portable binary format. Integers are
little-endian and floats are IEEE 754 bit patterns regardless of host
architecture. The body is a sequence of framed sections (tag, length,
payload) with lengths bounds-checked against the remaining file size and
unknown tags skipped, followed by a trailing FNV-1a digest over the whole
file. Snapshots persist the mutable language graph and neural state together
with the mirrored observation ledger, episodic memory, familiarity values,
counters, and PRNG state. v4 snapshots load portably and migrate to v5 on the
next save; v1-v3 are refused.

The observation ledger is separate from the snapshot and is the authority for
which external observations have been committed. It records source identity,
author identity where available, observation time, content digest, schema
version, outcome, and conversation context (reply root/parent and quote target,
metadata that is durable but never learnable). Its in-memory deduplication index
is rebuilt from the log when opened, and a replay rebuild restores conversation
context alongside learned state.

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

`atperson audit "hello world"` optionally sends the same bounded decision
trace to TypeSafe System One for a non-authoritative, operator-facing advisory
judgment (see `docs/audit.md`). It requires `ATPERSON_TYPESAFE_API_KEY`, is
opt-in, and never affects learned state or outbound policy.

To inspect the entity's own outbound experience and applied valence:

```sh
./build/atperson journal actions
./build/atperson journal events
./build/atperson journal valence
./build/atperson journal apply moon action 0.5 <action-id>
```

`apply` writes one explicit valence event and journals it, so a later
`rebuild` replays it after the ledger. See
[`docs/action-journal.md`](docs/action-journal.md).

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

To run the same ingestion continuously with retry backoff and a snapshot
cadence:

```sh
./build/atperson daemon          # until stopped (SIGINT/SIGTERM or control shutdown)
./build/atperson daemon 5        # bounded: stop after 5 cycles
```

The daemon holds the writer lock for its whole lifetime and re-reads operator
control every cycle, so `atperson control pause` and `atperson control
shutdown` work while it runs. Transport failures back off rather than busy
loop; a fatal local persistence error stops the process instead of continuing
on uncertain state. See [`docs/daemon.md`](docs/daemon.md) for the scheduling
and backoff environment knobs.

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

`sync` remains an explicitly-invoked, bounded run; `daemon` wraps the same
traversal in a continuous loop. Ingestion is read-only: the runtime does not
autonomously like, follow, reply, repost, or publish. The only write path is
the operator-led `publish` below, which must clear the outbound policy and the
operator `control` gate before a single record is written.

### Outbound action policy

An accepted core plan is evidence, never permission. The outbound layer gives
the runtime its own action vocabulary (`post`, `reply`, `like`, `repost`,
`follow`, `unfollow`, `moderation`) and evaluates operator-authored policy and
durable rate budgets before any write. It is **default-deny**: a missing
policy file disables every kind. Decisions are `allow`, `deny` or `defer`
with stable reason codes (`kind_disabled`, `unsupported_kind`,
`duplicate_suppressed`, `cooldown_active`, `window_exhausted`), and every
decision carries the budget state behind it. Rate budgets survive restart, so
a crash cannot reset a window and permit a burst.

```sh
./build/atperson outbound status          # enabled/disabled and current budget per kind
./build/atperson outbound rules           # the effective policy document
./build/atperson outbound evaluate reply at://did:plc:…/app.bsky.feed.post/… <digest>
./build/atperson outbound admit reply at://did:plc:…/app.bsky.feed.post/… <digest>
```

`evaluate` is read-only; `admit` consumes budget only on `allow` and
persists it. Both report the operator `control` gate and perform no network
writes. See [`docs/outbound-policy.md`](docs/outbound-policy.md).

### Outbound execution

`atperson publish <action-file>` is the one path from an approved decision to a
network write. It executes a frozen `atperson-outbound-action` v1 document —
exact text, frozen record key, #22 approval digest — through the ordered gates
(operator pause, #23 policy and rate budget, dry-run, #22 control approval) and
only then writes the record through Wolfram:

```sh
./build/atperson publish action.json
```

Replies resolve their parent/root CIDs from the live records at execution time.
The write uses `putRecord` under the frozen rkey, so a retry after an ambiguous
failure replaces the same record instead of duplicating it, and the rate budget
is consumed only on a confirmed success. Every attempt lands in a
credential-free append-only audit log. See
[`docs/outbound-execution.md`](docs/outbound-execution.md).

Mutating commands (`ingest`, `ingest-file`, `sync`, `cursor reset`,
`rebuild`, `compact`, `withdraw`, `daemon`) take an exclusive writer lock on
the data directory before touching durable state, so two processes cannot
mutate the same state concurrently. A lock left behind by a dead process is
detected and reclaimed automatically. Read-only commands and operator
`control` run without the lock and see state as of their own read — a
concurrent writer may commit after the reader started.

## Current boundaries

`atperson` intentionally does **not** currently:

- read private messages or private data;
- autonomously like, follow, reply, repost, or publish — `publish` is
  operator-led, gated and audited, never a side effect of learning;
- use an LLM as a hidden personality or decision engine;
- seed a biography, ideology, preferences, or opinions into learned state;
- equate generated language with consciousness or personhood;
- pretend source deletion and learned-contribution removal are solved.

The next meaningful work is not simply "let it post". The action model needs
higher-level sequence planning and explicit network policy. Autonomous output
should sit on top of those observable mechanisms, not bypass them.

## Licence

AGPL-3.0. See [`LICENSE`](LICENSE).
