# Architecture

The architecture starts with one rule:

```text
C23 = authoritative learned state
C++23 = runtime, integration, policy, and presentation
```

That boundary is deliberate. The learned graph must remain usable, testable,
serialisable and deterministic without requiring the network runtime, and the
runtime must not quietly become a second authority for what the entity knows.

The other important boundary is just as simple: AT Protocol mechanics belong to
[Wolfram](https://github.com/ewanc26/wolfram). `atperson` consumes them; it does
not grow a parallel protocol stack of its own.

## C23 learning core

`atperson_core` owns state whose meaning comes from learning or durable replay:

- token vocabulary;
- per-token embeddings;
- directed association edges;
- neural-network weights and biases;
- online gradient updates;
- exposure and training counters;
- source hashes attached to learned edges;
- episodic memory;
- familiarity;
- experience-derived valence;
- inspectable action candidates, plans and guarded decisions;
- structured planner-context selection;
- PRNG state;
- the observation ledger;
- snapshot persistence and replay compatibility.

A new graph contains no tokens or edges. Tokens are interned only when an
observation introduces them. There is no seeded vocabulary, biography,
personality, ideology, preference set or opinion table.

The neural component is deliberately bounded and inspectable. A brand-new
model generation selects a validated embedding width and hidden-layer topology
from the current hardware capacity recommendation (or an explicit operator
override) and persists that descriptor in snapshot v6. From then on the
persisted topology is authoritative: moving to a different host never silently
reshapes learned state. Once a generation is explicitly expanded, snapshot v7
also persists the ordered migration history and exact ledger boundaries needed
to reproduce those topology changes. The feed-forward scorer consumes a source/target
embedding pair, uses one to three runtime-selected `tanh` hidden layers, and
predicts whether the pair belongs together. Observed adjacent pairs are
positive examples; another known token is sampled as a negative example when
possible. Both the shared scorer and token embeddings train online with
gradient descent.

This is a learning primitive rather than a pretrained language model. It gives
the rest of the system a persistent, experience-shaped substrate without
hiding behaviour in a persona prompt.

## C++23 runtime

The C++ layer owns concerns that should not become learned state:

- process lifecycle;
- filesystem and configuration;
- RAII wrappers around C23 state;
- AT Protocol sessions through Wolfram;
- JSON extraction and record translation;
- scheduling and long-running daemon control;
- the runtime ingestion cursor;
- the state-directory writer lock;
- operator control state;
- outbound action vocabulary, default-deny policy and durable rate budgets;
- the operator-led `publish` execution path;
- the action/outcome journal and its event linkage;
- CLI and presentation.

The split is not “C does algorithms, C++ does everything else”. The important
question is authority. If a value is part of what the entity has learned, or is
needed to reproduce that learned state, it belongs in the C23 core. If it is
operator policy, process state, transport or orchestration, it belongs outside
that core.

`LanguageGraph` is intentionally a thin RAII wrapper over the C API; the
`Ledger` wrapper follows the same rule for the C23 observation ledger.

## First-run bootstrap

`atp_bootstrap_home` (`src/core/bootstrap.c`) runs before any command. When the
data directory or `.env` file does not exist it creates them — the directory at
mode `0700`, the `.env` template at mode `0600`.

The template documents every environment variable `atperson` reads. The
operator fills in credentials and sources it; `atperson` does not parse `.env`
itself.

Bootstrap is idempotent. Existing files and directories are never modified,
and a run that creates nothing reports nothing. It contains no learning logic
and no network access.

## State-directory writer lock

The snapshot, ledger, commit marker and ingestion cursor form one logical state
set. State-mutating commands such as `ingest`, `ingest-file`, `sync`, `cursor
reset`, `rebuild`, `compact`, `withdraw`, `daemon`, `journal apply` and
`journal map` acquire an exclusive lock on the data directory before touching
that set.

The lock is a `.writer-lock` file created with `O_CREAT|O_EXCL`, recording the
owner pid, a boot marker and acquisition time. Release removes the file. RAII
guarantees release on normal exit, exception or stack unwind. The daemon holds
the lock for its entire lifetime, not once per cycle, so another process cannot
load stale learned state and later overwrite a newer snapshot.

Operator `control` deliberately does **not** take the writer lock. Control state
is runtime metadata outside the learned-state set, and an operator must be able
to pause or request shutdown while a daemon owns the lock. Control saves are
atomic.

The `outbound` policy and rate-budget commands are also runtime metadata. They
can inspect and mutate policy/budget state while the daemon owns the writer
lock; see [`outbound-policy.md`](outbound-policy.md).

`atperson publish` does not take the writer lock either. It serialises its own
outbound read-modify-write path with a dedicated `.outbound-lock`, so a publish
does not need to stop long-running ingestion. The write still has to clear
operator control, outbound policy and approval gates before Wolfram is allowed
to touch the network. See [`outbound-execution.md`](outbound-execution.md).

A lock whose owner pid is dead, or whose boot marker differs from the current
boot, is stale and can be reclaimed. A lock held by a live process is respected.
An empty lockfile — the small window between creation and metadata write — gets
a short grace period before being treated as abandoned.

Read-only commands run without the writer lock and observe durable state as of
their own read. A concurrent writer may commit afterwards; readers do not block
writers and writers do not wait for readers.

## Wolfram boundary

AT Protocol networking belongs to
[`ewanc26/wolfram`](https://github.com/ewanc26/wolfram).

`atperson` consumes Wolfram's C23 implementation and C++ ownership layer. It
must not grow parallel implementations of XRPC, sessions, DID/handle
resolution, repository operations or Bluesky procedures.

There are currently two network directions:

- **ingestion** uses Wolfram to authenticate and fetch timeline pages, then
  translates public records into observations for the C23 core;
- **operator-led outbound execution** uses Wolfram only after an approved frozen
  post/reply action clears runtime policy and control gates.

The daemon itself remains read-only with respect to the network. Autonomous
posts, replies, likes, follows, reposts, DMs and moderation are not side effects
of learning or ingestion.

## Ingestion policy

The ingestion policy is the single reviewable decision point for which fetched
records may become observations. It consumes already-extracted fields — record
type, author DID, text, embed type, viewer state and feed reason — and produces
an eligibility decision or a machine-readable skip reason. Protocol mechanics
stay in Wolfram and the extraction layer; policy never touches the wire.

Rule order is deliberate:

1. **Record type.** Only `app.bsky.feed.post` records are supported. Other
   record types are skipped as `unsupported-record` rather than mis-parsed.
2. **Self-observation.** The authenticated account's own output is not learned
   from (`self-authored`). Feeding output straight back into input would create
   a self-reinforcing loop.
3. **Moderation and relationship state.** Viewer-blocked, viewer-blocked-by,
   viewer-muted and moderation-filtered records are skipped with explicit
   reasons.
4. **Text presence.** Empty text is `empty-text`; image/video-only posts with
   no text are `non-text-only`.

Replies and reposts remain eligible when they carry learnable text and retain
their feed/conversation context.

### Ledger semantics for skipped items

A policy-skipped record is not silently dropped. It still enters the ledger
with outcome `SKIPPED`, so “observed but deliberately not learned” remains
distinguishable from “never fetched”. The `(source id + content digest)` dedup
index also applies to skipped material, making replay idempotent.

## Observation ledger

The ledger is a durable, append-only C23 log of observations presented to the
learning core. Per observation it records:

- stable source identifier / AT URI;
- author DID when available;
- observation time;
- content digest;
- learning-schema version;
- processing outcome;
- retained canonical observation bytes;
- conversation metadata (reply root, reply parent and quote target).

The ledger is the authority for committed external experience. A committed
`LEARNED` or `SKIPPED` observation is never trained on again across process
restarts. Outcome changes append patch records rather than rewriting history.
Because learnable payload bytes are retained, the ledger can reconstruct the
state rather than merely proving that an observation once existed.

### Files and layout

- The record log begins with `"ATPLDG03"` and a little-endian version `u32`.
  v1/v2 logs (`"ATPLDG01"` / `"ATPLDG02"`) migrate on open.
- Each record is `len u32 | crc u32 (FNV-1a 32) | type u8 | payload`, where
  type 1 is an observation entry and type 2 is an outcome patch.
- An entry body is `id u64 | source_len u32 | source | author_len u32 |
  author | observed_at u64 | digest u64 | schema u32 | outcome u8 |
  payload_len u32 | payload | root_len u32 | reply_root_uri |
  parent_len u32 | reply_parent_uri | quote_len u32 | quote_uri`.
- Payloads are length-prefixed bytes, never NUL-terminated. Retention is capped
  at `ATPERSON_LEDGER_PAYLOAD_LIMIT` (64 KiB); larger observations are refused
  before durable mutation.
- Conversation context is durable metadata, not learnable content. It is covered
  by the record CRC but excluded from the content digest. Each context URI is
  capped at `ATPERSON_CONTEXT_URI_BYTES` (256).
- `<path>.off` is the durable commit marker: `"ATPLOF02"`, version, committed
  count and committed byte offset (28 bytes total).

Multi-octet integers are little-endian. The ledger used little-endian encoding
from the start; snapshot v5 later adopted the same portable convention.

### Payload reads

`atp_ledger_entry_payload(ledger, id, out, capacity, out_len)` returns retained
bytes exactly as appended. A payload-less entry reports length 0 rather than
inventing content. Every read re-verifies the bytes against the stored digest;
a mismatch is `ATP_ERR_FORMAT`. Passing a null output buffer with capacity 0
queries the length without copying.

### Legacy migration

Opening a v1 or v2 log migrates it before recovery. Migration validates the
committed prefix, flattens outcome patches, streams transformed v3 records to a
temporary file, `fsync`s and renames atomically.

A crash before the rename leaves the old generation intact; after the rename
the v3 file is complete. v1 entries remain payload-less because their original
bytes were never retained. v1/v2 entries also gain empty conversation context,
which is the honest value for observations predating that metadata.

### Crash safety

Write ordering is part of the correctness contract:

1. write record bytes to the log and `fsync`;
2. stage the new marker in `<path>.off.tmp` and `fsync` it;
3. rename the temporary marker over `.off`.

A crash at any point leaves the previous committed prefix intact. Recovery
truncates torn tail bytes beyond the committed offset, removes stale staging
files and heals a missing marker from the longest valid log prefix. Corruption
inside the committed prefix is refused as a format error rather than silently
trimmed away.

### Deduplication

The unique `(source id + content digest)` index is rebuilt in memory on open and
maintained on append. Reservations use outcome `PENDING` before training.
`LEARNED` is written only after the observation successfully reaches the graph.
`PENDING` and `FAILED` remain retryable, so a crash during observation never
silently loses input.

### Runtime flow and snapshot mirror

The sync path is, conceptually:

```text
append(PENDING) -> observe -> set outcome -> mirror committed metadata
```

Eligible text becomes `LEARNED`; deliberately excluded material becomes
`SKIPPED`. The retained ledger payload is the replay authority. The snapshot
contains a mirror for inspection and fast restart, but rebuild does not trust
that mirror over the ledger.

Deleting learned contribution is handled by withdrawal plus rebuild, not an
approximate inverse gradient step.

### Deterministic rebuild

`atp_replay_ledger(ledger, graph, report)` reconstructs observation-derived
state from the ledger alone. Entries replay in ledger id order through the same
observe path used by ingestion, preserving the deterministic config seed, PRNG
stream and training decisions.

| Outcome | Replay behaviour |
| --- | --- |
| `LEARNED` | Payload re-observed; entry mirrored |
| `SKIPPED` | Mirrored only |
| `PENDING` | Excluded; still retryable |
| `FAILED` | Excluded; still retryable |
| `WITHDRAWN` | Excluded completely |

A `LEARNED` entry without a retained payload cannot be reproduced and fails the
rebuild with `ATP_ERR_FORMAT` instead of creating a different graph silently.

`atperson rebuild` takes the writer lock, replays the observation ledger into a
fresh graph, then replays explicit valence entries from the action journal in
append order. The replacement snapshot is saved atomically. A failed rebuild
leaves the previous snapshot untouched, and identical ledger+journal input
produces byte-identical rebuilt snapshots.

### Learning-schema compatibility

Each ledger entry records the learning schema that gave its bytes meaning.
Replay consults `atp_schema_can_replay(version)` before applying an entry, and
snapshot loading refuses schemas the current core cannot reproduce.

A format problem is `ATP_ERR_FORMAT`; a valid record whose learning semantics
are not supported is `ATP_ERR_SCHEMA`. Replay reports the first failing ledger
id and schema.

Schema changes fall into three classes:

| Class | Replay behaviour |
| --- | --- |
| Replay-compatible | Current observe path reproduces the old effect |
| Adapter migration | A versioned handler reproduces the old behaviour |
| Incompatible | Replay fails and a new model generation is required |

Mixed-schema ledgers replay in id order and stop at the first incompatible
entry. Old and new semantics are never silently blended.

Any change to tokenisation, sampling, memory selection, familiarity or training
equations therefore needs an explicit schema decision and transition tests.

## Source withdrawal and unlearning

`ATP_LEDGER_OUTCOME_WITHDRAWN` is an append-only ledger outcome. Withdrawal
never rewrites history and is idempotent.

Supported scopes are:

- `atp_ledger_withdraw(id)` — one observation;
- `atp_ledger_withdraw_source(source_id)` — all entries from one source URI;
- `atp_ledger_withdraw_author(author_did)` — all entries by one account.

Edited records need no special case because deduplication includes the content
digest. The same URI with different bytes is a new observation; withdrawing the
old digest does not block the edit.

Withdrawal does not mutate the current in-memory graph. The graph deliberately
has no inverse-observe operation: subtracting approximate neural updates would
not recover the state that would have existed without the source. Instead,
withdrawal changes the ledger outcome and the next rebuild reconstructs graph,
neural state, familiarity, memory, counters and snapshot mirror without those
entries.

Until rebuild, the live snapshot may still contain stale contribution. The
ledger remains authoritative.

## Ledger compaction

The append-only ledger accumulates entry records, outcome patches and retained
payloads. `atp_ledger_compact` reclaims bytes that are provably dead. It is
explicit and never runs automatically on open.

| Dropped | Why it is safe |
| --- | --- |
| Outcome patch records | Final outcomes are flattened into rewritten entry records |
| Withdrawn payloads | Replay excludes them and the tombstone still preserves deduplication |
| Nothing else | Learned payloads are replay input; other outcomes may still become learnable |

Compaction preserves ledger ids and logical outcomes exactly. Every entry
survives either in full or as a withdrawn tombstone, so episode/source linkage
and deduplication remain stable.

Crash safety uses an atomic-generation replacement:

1. stream the compacted log to `<path>.tmp`, `fsync`, close;
2. remove the commit marker;
3. rename the temporary log over the old one;
4. reopen and rewrite the marker from the compacted generation.

Recovery can always identify a complete valid generation. Rebuild equivalence
before and after compaction is a test invariant.

## Episodic memory

The ledger is the complete durable observation history; episodic memory is a
curated learned view of it.

An episode stores its ledger id, observation time, content digest, schema
version, source id, author DID and a compact summary of significant tokens.
Selection is inspectable and count-based: observations introducing new
vocabulary or containing at least two distinct tokens may be remembered;
empty/single-token repeats are not.

The summary keeps the top eight tokens by in-text count with deterministic tie
breaking. Association edges remain the semantic substrate; episodes are the
episodic substrate.

Two recall surfaces now exist:

- the original exact-overlap recall API;
- ranked recall, which can combine exact support, direct learned associations,
  familiarity, recency and previous use while preserving all component scores.

Actual recall updates recall counters. Planner-context selection uses the
read-only ranked-recall preview path so merely considering a memory does not
mutate it. See [`episodic-recall.md`](episodic-recall.md).

Eviction is deterministic and least-used-first, then oldest, then smallest
ledger id. Memory remains serialisable and replayable from the ledger.

### Ingestion cursor

`sync` consumes bounded timeline pages through Wolfram's cursor-aware timeline
API and persists interrupted catch-up state in
`~/.ewanc26/atperson/ingestion-state.json` (override with
`ATPERSON_INGESTION_STATE`).

The cursor is C++ runtime metadata, not learned state:

```text
observation ledger = authority for committed external experience
model snapshot     = durable learned state / fast restart
journal            = authority for recorded self-authored experience
cursor              = fetching checkpoint only
```

Every record in a page is durably processed before that page's cursor is
checkpointed. A crash mid-page therefore refetches the page and ledger
deduplication removes already committed work.

When traversal is exhausted, the cursor is cleared. A service-rejected cursor
is reported and discarded; the run can restart from the current head because
the ledger, not the cursor, defines what has already been learned. Cursor state
is bound to account DID, service URL and endpoint.

`checkpoint.generation`, `saved_at`, `pages_completed` and
`observations_seen` are operational telemetry only. Cursor persistence is
atomic.

## Familiarity and valence

Familiarity and valence are deliberately different signals.

**Familiarity** is exposure-derived and value-neutral:

```text
familiarity = familiarity * familiarity_decay + 1
```

A token begins at `1.0` on first exposure and rises towards
`1 / (1 - decay)` (`familiarity_decay` defaults to `0.98`). Familiarity says
how repeatedly present something has been; it does not say whether the entity
likes, dislikes, trusts or approves of it.

**Valence** is experience-derived and changes only through explicit events:

```text
valence' = valence + rate * (signal - valence)
```

Signals are bounded to `[-1, 1]`. Exposure alone never changes valence. Applied
valence events are journalled so rebuild can replay them after the observation
ledger. See [`valence.md`](valence.md) and
[`action-journal.md`](action-journal.md).

Neither familiarity nor valence grants network permission. They are learned
state; outbound permission remains runtime policy.

## Action model and planner

The learned action layer is entirely inspectable and read-only until an
operator/runtime policy explicitly moves an approved action into the outbound
path.

### Candidates

`atp_graph_action_candidates` tokenises supplied context without interning
unknown vocabulary, keeps distinct known context nodes and considers existing
outgoing associations. Targets supported by multiple context nodes are merged.

Each candidate exposes the values used for ranking:

```text
association_score = mean(0.7 * learned_edge_strength + 0.3 * neural_score)
familiarity_score = familiarity * (1 - familiarity_decay), clamped to 0..1
support_score     = supporting_observations / (supporting_observations + 1)
score             = mean(association_score, familiarity_score, support_score)
```

Results also include supporting observation counts and context-match counts.
Unknown context produces no candidate and inspection never mutates the graph.

### Bounded plans and guarded decisions

The C23 planner can compose bounded candidate sequences. Raw plans remain useful
for inspection even when they contain a cycle or weak tail. The guarded
decision layer applies explicit score, support, drop, repetition and cycle
rules and can either return a supported prefix or abstain completely.

An abstention is a successful decision that the learned evidence is
insufficient, not a runtime error.

See [`action-inspection.md`](action-inspection.md) and
[`action-termination.md`](action-termination.md) for the exact trace fields and
default thresholds.

### Structured context

`atp_graph_select_context` assembles deterministic planner context from explicit
sources: immediate input, caller-supplied recent interaction, ranked episodic
memory and neutral author/source interaction state.

The selector does not build a hidden prompt. Every selected item has a kind,
reason, score, provenance and budget contribution. Memory consideration uses a
read-only preview path, and author/source continuity is derived from existing
state rather than a second mutable relationship database.

See [`planner-context.md`](planner-context.md).

### Policy boundary

A C23 plan is evidence, never network permission.

The C++23 outbound layer owns action kinds, default-deny policy, durable rate
budgets and operator control. `atperson publish` currently executes frozen,
operator-approved posts and replies through Wolfram only after those gates pass.
Other autonomous social behaviour remains fail-closed.

## Persistence

Snapshots are versioned and contain the complete mutable graph/neural state,
counters and deterministic PRNG state, together with mirrored ledger metadata,
episodic memory, familiarity, valence and conversation context.

The portable snapshot family uses little-endian integers, IEEE 754 float bit
patterns, framed sections (`tag u32le | length u64le | payload`) with
bounds-checked lengths and skippable unknown tags, followed by a trailing
FNV-1a digest over the entire file. v6 persists the explicit neural
architecture. v7 is reserved for generations that have actually expanded and
adds an ordered migration-history section containing each migration's algorithm
version, deterministic seed, ledger boundary and source/target architecture.

Writers keep the oldest format that can represent the generation honestly:
legacy graphs remain byte-compatible v5, variable-topology graphs without
migration history remain v6, and only migrated generations become v7. This is
also a compatibility guard: older software that does not understand migration
boundaries rejects v7 instead of rebuilding the entire ledger at the final
topology and silently producing different learned state. v4/v5 snapshots still
load at the legacy neural topology; v1-v3 are refused rather than silently
reinterpreted.

The snapshot is a durable fast-start representation of learned state. The
observation ledger and action journal remain the evidence streams from which a
rebuild derives that state.

## Implementation state

The original staged plan has mostly turned into implemented architecture:

1. **Language graph** — implemented: vocabulary, embeddings, associations,
   neural scoring and persistence.
2. **Observation ledger** — implemented: durable deduplication, replay,
   provenance, withdrawal, compaction and schema compatibility.
3. **Memory** — implemented first pass: source-linked episodic memory, ranked
   recall and deterministic eviction.
4. **Learned internal state** — implemented: exposure-derived familiarity and
   explicit experience-derived valence.
5. **Action model** — implemented first pass: candidates, bounded plans,
   abstention/termination guards and structured context selection.
6. **Runtime policy** — implemented: operator control, default-deny outbound
   policy, durable rate budgets and audit surfaces.
7. **Outbound execution** — implemented narrowly for operator-led frozen posts
   and replies through Wolfram; autonomous social behaviour remains disabled.
8. **Long-running runtime** — implemented: daemonised bounded ingestion cycles,
   retry backoff, snapshot cadence and graceful shutdown.

The remaining work is not “turn autonomy on”. Any broader autonomous behaviour
has to preserve the same inspectability, provenance, rate control and
fail-closed boundaries already used by the operator-led path.

No feature should skip observability just to make the entity appear more human.

## Growth bounds

The semantic graph grows with observed vocabulary. Two mechanisms keep that
growth bounded and fast while preserving the canonical state arrays as the
source of truth.

### Hash indexes

Node lookup (`token -> index`) and edge lookup (`(source, target) -> index`) use
open-addressing hash tables over the canonical arrays: power-of-two capacity,
linear probing, load factor at or below 0.5 and `UINT32_MAX` as the empty
marker.

Indexes are maintained incrementally on intern/edge creation, rebuilt by
snapshot loaders and never persisted. The packed `(source << 32 | target)` edge
key runs through a splitmix64-style finaliser before probing; direct identity
hashing caused dense sequential keys to degrade into long probe runs.

With the finaliser, `atp_find_node` and `atp_find_edge` are O(1) expected.
Association inspection still scales with the queried node's out-degree because
all candidate edges for that node must be ranked.

### Resource ceilings

`atp_graph_config.node_capacity_max` and `edge_capacity_max` (`0` = unlimited)
are resource budgets, not retention policy. Before observation mutation, a
dry-run pass counts new nodes and edges. If the observation would cross a
ceiling, the whole observation is rejected with `ATP_ERR_CAPACITY`; partial
learning is not allowed.

Ceilings are deployment/runtime policy rather than persisted graph semantics.
Loading a graph restores its learned state and the runtime reapplies current
capacity policy afterwards. Lowering a ceiling below current counts evicts
nothing; it only blocks further growth.

Vocabulary pruning is separate future work because episodes refer to graph
nodes by stable index. The safe current behaviour is fail-closed growth, not
silent deletion.

### Benchmarks

`tests/bench.c` (`ctest -L bench`) measures deterministic synthetic histories
with fixed seeds and skewed vocabulary. It covers observation throughput,
association lookup, recall, snapshot save/load and a memory-footprint estimate.
Timing is reported but never asserted, so CI variance cannot make the suite
flake.

Representative M2 `-O2` figures from September 2026:

| Profile | Nodes | Edges | Observe µs/obs | Lookup µs/query | Snapshot save/load |
| --- | ---: | ---: | ---: | ---: | ---: |
| small | 500 | 1.6k | 20 | 6.3 | 0.9ms / 0.8ms |
| medium | 4.9k | 15.8k | 27 | 28 | 4.3ms / 4.5ms |
| large | 24.7k | 78.2k | 44 | 128 | 13.7ms / 16.1ms |

Observe cost grows with the number of pairs in an observation rather than a
linear scan of graph history. Lookup cost grows with the queried node's
out-degree, not total graph size.

## Tokenisation contract

Token identity is durable learning state: vocabulary, edges, episodes and
snapshots all key on token bytes. Tokenisation is therefore versioned through
`ATPERSON_SCHEMA_VERSION`; changing it is a schema decision rather than an
incidental parser refactor.

All token-producing paths — observation, action-context scanning, recall and
association lookup — use `src/core/tokenize.c` / `atp_tokenize`. The schema
version selects the correct scanner so older ledger entries keep their original
learning meaning during replay.

### Schema 1 (legacy)

Schema 1 is byte-oriented and preserved for replay. ASCII is lowercased, bytes
`>= 0x80` are token bytes, `'`, `-` and `_` are token bytes, and everything
else separates. Tokens cap at 95 bytes with the historical truncation rules.

### Schema 2 (Unicode)

| Decision | Rule |
| --- | --- |
| UTF-8 validation | Invalid sequences become U+FFFD, which is a separator |
| Normalisation | NFKC_Casefold + LUMP (`utf8proc`) |
| Case handling | Unicode case folding across supported scripts |
| Token bytes | Unicode L*, M*, N* categories plus `'`, `-`, `_` |
| Separators | Punctuation, symbols, whitespace, control, emoji and U+FFFD |
| Emoji | Separators, avoiding vocabulary explosion from ZWJ/modifier variants |
| Combining marks | Preserved as token bytes |
| URLs/handles | Split at punctuation such as `.`, `:`, `/`, `@` |
| Truncation | At a codepoint boundary, never mid-sequence |

Canonical-equivalence examples include:

- NFC `café` and decomposed `cafe` + U+0301;
- `Café` and `CAFÉ`;
- `ﬁ` -> `fi`;
- Kelvin sign `K` (U+212A) -> `k`;
- curly-apostrophe `don’t` -> `don't` through LUMP.

`utf8proc` is MIT-licensed pure C, keeping normalisation, case folding and UTF-8
validation inside the C23 core without introducing a C++ or Wolfram dependency.
