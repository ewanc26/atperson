# Architecture

atperson has one hard boundary:

```text
C23 = authoritative learned state
C++23 = runtime, integration, policy, and presentation
```

The distinction is intentional. The entity's learned language graph must remain
usable, testable, serialisable, and deterministic without requiring the network
runtime.

## C23 learning core

`atperson_core` owns all state that changes because of learning:

- token vocabulary;
- per-token embeddings;
- directed association edges;
- neural-network weights and biases;
- online gradient updates;
- exposure and training counters;
- source hashes attached to learned edges;
- episodic memory;
- internal state (per-token familiarity);
- inspectable action-candidate scoring over learned state;
- PRNG state;
- the observation ledger;
- persistence format.

A new graph contains no tokens or edges. Tokens are interned only when an
observation introduces them.

The initial neural component is deliberately small. Each token receives a
16-dimensional embedding. A feed-forward scorer takes the source and target
embeddings, passes their concatenation through a 16-unit `tanh` hidden layer,
and predicts whether the pair belongs together. Observed adjacent pairs are
positive examples; another known token is sampled as a negative example when
possible. Both the shared scorer and token embeddings train online with
gradient descent.

This is a learning primitive, not a finished language model. It gives later
systems a persistent, experience-shaped substrate without baking in a
pretrained model or scripted personality.

## C++23 runtime

The C++ layer owns concerns that should not become part of the model itself:

- process lifecycle;
- filesystem and configuration;
- RAII;
- AT Protocol sessions;
- JSON extraction;
- scheduling;
- protocol event translation; the C++ sync path feeds the ledger, which owns
  the deduplication;
- the runtime ingestion cursor (see below);
- the state-directory writer lock (see below);
- future network-action policy, rate limiting, and presentation. Learned
  candidate scoring remains in C23.

`LanguageGraph` is intentionally a thin RAII wrapper over the C API; the
`Ledger` wrapper is the same for the C23 observation ledger.

## First-run bootstrap

`atp_bootstrap_home` (C23 core, `src/core/bootstrap.c`) runs before any
command: when the data directory or `.env` file does not exist it creates
both — the directory at mode `0700`, the `.env` template at mode `0600`. The
template documents every environment variable atperson reads; the operator
fills in credentials and sources it. atperson never parses `.env` itself —
it reads the environment, the file is operator convenience only.

The bootstrap is idempotent: existing files and directories are never
modified, and a run that creates nothing reports nothing. It contains no
learning logic and no network access; it is environment preparation, so it
lives in the core layer where it can be tested without the runtime.

## State-directory writer lock

The snapshot, ledger, commit marker, and ingestion cursor form one logical
state set. Every mutating command (`ingest`, `ingest-file`, `sync`,
`cursor reset`) acquires an exclusive lock on the data directory before
touching durable state: a `.writer-lock` file created with `O_CREAT|O_EXCL`,
recording the owner's pid, a boot marker, and the acquisition time. Release
removes the file; RAII guarantees release on normal exit, exception, or
stack unwind.

Stale detection: a lockfile whose owner pid is dead, or whose boot marker
differs from the current boot (the machine rebooted), is provably stale and
reclaimed. A lockfile owned by a live process is respected — acquisition
fails with a diagnostic naming the holder. An empty lockfile (the
microsecond window between create and metadata write) is given a short
grace period, then treated as abandoned.

Consistency model for readers: read-only commands (`stats`, `assoc`,
`candidates`, `familiarity`, `recall`, `cursor status`) run without the lock
and observe the snapshot and ledger as of their own read. A concurrent
writer may commit after a reader started; readers never block writers and
writers never wait for readers. This is the documented trade-off until a
shared-lock reader path is needed.

## Wolfram boundary

AT Protocol networking belongs to
[`ewanc26/wolfram`](https://github.com/ewanc26/wolfram).

atperson consumes Wolfram's C23 implementation and C++ RAII ownership layer. It
must not grow parallel implementations of XRPC, session management, DID/handle
resolution, repository operations, or Bluesky procedures.

The first network path uses `wf_agent_login` and `wf_agent_get_timeline`, then
extracts public post text and passes each post to the learning core with its AT
URI as the source identifier.

## Ingestion policy

The ingestion policy (`src/app/ingestion_policy.{hpp,cpp}`) is the single
reviewable decision point for which fetched records may become observations.
It consumes already-extracted fields — record type, author DID, text, embed
type, viewer state, feed reason — and produces a decision: eligible, or
skipped with a machine-readable reason. Protocol mechanics stay in Wolfram
and the client; the policy never touches the wire.

Rule order is deliberate:

1. **Record type.** Only `app.bsky.feed.post` records are supported. A feed
   can carry other record types; they are skipped as `unsupported-record`
   rather than mis-parsed.
2. **Self-observation.** The authenticated account's own output is never
   learned from (`self-authored`). This is an explicit policy decision:
   learning from self-authored records would create a feedback loop between
   the entity's output and its experience.
3. **Moderation and relationship state.** Viewer-blocked, viewer-blocked-by,
   viewer-muted, and moderation-filtered posts are skipped
   (`viewer-blocked`, `viewer-blocked-by`, `viewer-muted`,
   `moderation-filtered`). The account chose not to see this content;
   atperson respects that choice.
4. **Text presence.** Empty text is skipped (`empty-text`); image- or
   video-only posts with no text are skipped as `non-text-only`.

Replies and reposts remain eligible and are tagged (`reply`, `repost`): a
repost's feed item points at the underlying post, and a reply carries its own
text. Text is text.

### Ledger semantics for skipped items

A policy-skipped item is not silently dropped. It still flows through the
ledger as an observation with outcome `SKIPPED`, so "observed but not
learned" stays distinguishable from "never fetched" — the ledger distinguishes
skipped from unfetched data. Replaying the timeline is idempotent for
skipped items too: the `(source id + content digest)` dedup index suppresses
re-processing, so a replay counts them as duplicates rather than fresh skips.

## Observation ledger

The ledger is a durable, append-only C23 log of every observation fed to the
learning core. It records, per observation:

- AT URI / stable source identifier;
- author DID where applicable;
- observed-at time;
- content digest;
- model/schema version;
- processing outcome;
- the canonical observation bytes (inline payload, format v2).

The log is the authority for what the entity has seen. A committed observation
(`LEARNED` or `SKIPPED`) is never trained on again across process restarts;
outcome changes append a small patch record rather than rewriting log bytes.
Because the canonical bytes are retained inline, the ledger is replayable: a
rebuild can recover the exact content that produced the current graph, not
just the fact that it was observed.

### Files and layout

- `path` is the record log: `"ATPLDG02"` then a little-endian version u32
  (v2; v1 logs with the `"ATPLDG01"` magic migrate on open, see below).
- Each record is `len u32 | crc u32 (FNV-1a 32) | type u8 | payload`, where
  type 1 is an observation entry and type 2 is an outcome patch.
- An entry body is `id u64 | source_len u32 | source | author_len u32 |
  author | observed_at u64 | digest u64 | schema u32 | outcome u8 |
  payload_len u32 | payload`. `payload_len` 0 marks a payload-less entry.
- Payloads are length-prefixed bytes, never NUL-terminated; binary content
  round-trips exactly. Retention is capped at `ATPERSON_LEDGER_PAYLOAD_LIMIT`
  (64 KiB); larger observations are rejected before any durable write.
- `<path>.off` is the durable commit marker: `"ATPLOF02"`, version, committed
  count, and the byte offset of the committed prefix (28 bytes total).

Multi-octet integers are little-endian, a deliberate format decision: snapshot
v1 stays host-oriented, and the two files may migrate independently in a future
version bump.

### Payload reads

`atp_ledger_entry_payload(ledger, id, out, capacity, out_len)` returns the
retained bytes exactly as appended. A payload-less entry (v1-migrated, or an
empty observation) reports honest absence: `ATP_OK` with length 0, never a
fake empty string presented as data. Every read re-verifies the payload
against the entry's content digest; a mismatch is `ATP_ERR_FORMAT`, so
corrupted bytes are never returned as content. A null `out` with capacity 0
queries the length without copying.

### v1 migration

Opening a v1 log migrates it before recovery. The migration validates the v1
committed prefix, flattens outcome patches onto their entries, streams the
transformed v2 records to a temp file, fsyncs, and renames atomically. A crash
before the rename leaves the intact v1 log; after it, the v2 log is complete.
The stale v1 marker is discarded and rewritten from the migrated log.

Patch history flattens to final outcomes — intermediate PENDING/FAILED states
are not preserved across migration. v1 entries migrate payload-less: their
observation bytes were never retained, and payload reads report that honestly
rather than faking content.

### Crash safety

Write ordering is the correctness argument:

1. write the record bytes to the log and `fsync`;
2. stage the new marker in `<path>.off.tmp` and `fsync` it;
3. rename the temp marker over `.off`.

A crash at any point leaves the previous committed prefix intact. Recovery
truncates a torn tail beyond the committed offset, removes stale staging files,
and heals a missing marker from the longest valid log prefix. A marker pointing
beyond the log, or checksum damage inside the committed prefix, is refused as a
format error rather than silently truncated.

### Deduplication

The unique `(source id + content digest)` index is rebuilt in memory on open
and maintained on append. Reservations are made (outcome `PENDING`) before any
training happens; the sync path flips them to `LEARNED` only after an
observation reaches the graph. `FAILED` and `PENDING` reservations are
retryable, so a crash mid-observe never silently drops an observation.

### Runtime flow and snapshot mirror

Each synced post: `append(source, author, observed_at, digest, schema,
PENDING, text)` -> observe -> `set_outcome(id, LEARNED)` (or `SKIPPED` for
empty text), then mirror the entry into the graph snapshot (v2) so recent
provenance is inspectable in one file. The retained text makes the ledger
replayable; the ledger remains authoritative for rebuilds.

Deleting a learned contribution is solved by withdrawal plus rebuild (see
"Source withdrawal and unlearning" below): the model is
rebuild-from-ledger, never approximate inverse gradient steps.

### Deterministic rebuild

`atp_replay_ledger(ledger, graph, report)` reconstructs learned state from
the ledger alone — no network access, no snapshot required. Entries are
re-applied in ledger id order (the order they were originally observed in)
through the same observe path sync uses, so a rebuilt graph is what the
original run would have produced from the same bytes: same config seed, same
PRNG stream, same training decisions.

Outcome semantics are explicit:

| Outcome | Replay behaviour |
|---------|------------------|
| `LEARNED` | Payload re-observed (training, episodic memory, familiarity), entry mirrored |
| `SKIPPED` | Mirrored only — observed but deliberately not learned, as the original run decided |
| `PENDING` | Excluded — retryable reservation, not committed experience |
| `FAILED` | Excluded — examined but untrainable, retryable |
| `WITHDRAWN` | Excluded — durably removed; the rebuilt state never saw it |

A LEARNED entry without a retained payload (a v1-migrated ledger) cannot be
replayed — the training input is gone — and fails the whole rebuild with
`ATP_ERR_FORMAT` and `report->failed_at_id` set, rather than silently
producing a graph that never saw those bytes. Schema versions the core does
not implement fail the same way instead of being reinterpreted.

The CLI surfaces this as `atperson rebuild`: it takes the writer lock,
replays the ledger into a fresh graph, and saves the result atomically
(tmp + fsync + rename, the same path `atp_graph_save` always uses). A
failure at any point — unreplayable entry, digest mismatch — leaves the
previous snapshot untouched; the rebuilt snapshot replaces it only on
success. Two rebuilds of the same ledger produce byte-identical snapshots.

## Source withdrawal and unlearning

Learned contributions are not permanent. `ATP_LEDGER_OUTCOME_WITHDRAWN` is
a fifth ledger outcome applied through the existing append-only patch
records: withdrawal never rewrites log history, is idempotent (withdrawing
an already-withdrawn entry is a no-op), and the patch sequence on disk is
the audit trail.

Three scopes:

- `atp_ledger_withdraw(id)` — one observation;
- `atp_ledger_withdraw_source(source_id)` — every entry from one source
  URI (a deleted AT record);
- `atp_ledger_withdraw_author(author_did)` — every entry by one account.

Edited records need no special machinery: the dedup index keys on
`(source id + content digest)`, so the same URI with new content appends a
fresh entry. Withdrawing the old content excludes it while the edit trains
normally.

Withdrawal never mutates the live graph. The graph has no inverse-observe,
and approximate subtraction from neural parameters would not restore the
state that would have existed without the source — the architecture
explicitly rejects that. Instead, withdrawal patches the ledger, and the
next rebuild produces the corrected state atomically: withdrawn entries
are excluded from graph, neural training, familiarity, memory, counters,
and the snapshot mirror, so the rebuilt state is what the entity would
have been without them. Already-evicted episodic memories need no special
handling — eviction is deterministic from replay order, so a rebuilt graph
never contains episodes from withdrawn observations. The live snapshot
keeps stale state until the next rebuild; the ledger is the authority, the
snapshot is a cache.

A withdrawn entry is committed history: it blocks re-append (the
observation stays deduplicated) and cannot regress to `PENDING`. The CLI
surfaces this as `atperson withdraw <id|source|author> <target>`, which
prints a reminder to run `atperson rebuild` to apply the withdrawal to
learned state.

## Episodic memory

The ledger is the durable, complete record; memory is a curated facet of it.
An **episode** is a remembered observation, not every observation. Each episode
stores its ledger id, observed-at time, content digest, schema version, source
id and author DID, and a compact summary of the most significant tokens.

Selection is purely count-based and inspectable — no hidden thresholds on
meaning, sentiment, or topic:

- an observation is remembered when it **introduces new vocabulary** (a new
  node was interned) or contains **at least two distinct tokens**;
- empty or single-token repeat observations are not remembered.

The summary holds the top eight tokens by in-text count (weights are raw
counts, ties broken by node index). The weighted association edges remain the
semantic substrate; episodes are the episodic substrate.

**Recall** tokenises the query without mutating the vocabulary, scores each
episode by the sum of its summary-token weights that overlap the query, and
returns matches strongest-first with recency then ledger id breaking ties.
Recalled episodes get their `recall_count` incremented and `last_recall_at`
updated, so consolidation is observable and drives eviction.

**Eviction** is deterministic and least-used-first: when memory is at capacity,
the episode with the lowest recall count is evicted (then oldest, then smallest
ledger id), and a `episode_evictions` counter is bumped. Everything is
serialisable, so memory survives restarts byte-for-byte.

The snapshot stores the episodic-memory block (snapshot v3) after the mirrored
ledger block, so the same file that is authoritative for the graph also carries
the remembered-view provenance.

### Runtime flow (sync)

Each synced post: `append(source, author, observed_at, digest, schema,
PENDING, text)` -> `remember(...)` -> `set_outcome(id, LEARNED)` (or `SKIPPED`
for empty text), then the entry is mirrored into the graph snapshot (v2) and
the episode (if selected) is stored in memory (v3). The retained text makes
the ledger replayable; the ledger remains authoritative for rebuilds, and
memory is linked to it by ledger id.

### Ingestion cursor

`sync` consumes bounded timeline pages through Wolfram's cursor-aware
`wf_agent_get_timeline` and persists an **interrupted catch-up checkpoint** in
`~/.ewanc26/atperson/ingestion-state.json` (versioned JSON format
`atperson-ingestion-state`, override with `ATPERSON_INGESTION_STATE`). The
cursor is C++ runtime metadata, never learned C23 state: it is not part of
the model snapshot and cannot influence the graph.

The authority hierarchy is explicit:

```text
observation ledger = authority for what has been committed
model snapshot     = durable learned state
ingestion state    = fetching optimisation/checkpoint only
```

The AT Protocol cursor is opaque — it is read, persisted, and passed back to
Wolfram, never parsed or compared. Ordering invariant: every observation in a
page is durably processed before that page's cursor is checkpointed, so a
crash mid-page refetches the same page on restart and ledger deduplication
suppresses anything already committed. A failed run advances nothing.

When a traversal reaches exhaustion the cursor is cleared; the next
independent sync starts at the current timeline head again and the ledger
filters already-seen `(source id, digest)` pairs, so an expired cursor can
never cause new head posts to be skipped. A cursor the service rejects is
reported, discarded, and the run restarts from the head — a fetching-state
failure, never a model-state failure. The cursor is bound to the
authenticated account DID, service URL, and endpoint, so a cursor from
another account or service is never reused.

`checkpoint.generation` (monotonic), `saved_at` (RFC 3339 UTC),
`pages_completed`, and `observations_seen` are operational telemetry only.
`atperson cursor status` inspects the checkpoint; `atperson cursor reset`
explicitly clears it. Persistence is atomic (temp file + rename), and a
leftover `.tmp` file never takes precedence over the committed state.

## Internal state

Internal state is the first, conservative pass at "preferences/values": a
slowly learned, experience-derived **familiarity** score per token. It is an
exponentially weighted exposure count:

```text
familiarity = familiarity * familiarity_decay + 1    on every exposure
```

A token starts at 1.0 on first sight and rises toward `1 / (1 - decay)`
(`familiarity_decay` defaults to 0.98, configurable per graph). It is pure
state derived from repetition — there is no value, polarity, sentiment, or
topic judgment attached, so it cannot encode a hidden opinion. It is updated
in the same C23 intern path as every observation, is queryable read-only via
`atp_graph_familiarity` (and the CLI `familiarity <token>`), and is persisted
as the snapshot v4 familiarity block. This score can inform recall and action
scoring as a purely behavioural signal.

## Action model

Stage 5 begins with a deliberately read-only continuation-candidate scorer in
C23. It does **not** generate a post, choose a Bluesky operation, or perform any
network side effect. Its job is to expose a deterministic planning surface over
state the entity has actually learned.

`atp_graph_action_candidates` (declared in `include/atperson/action.h`) tokenises
the supplied context without mutating the vocabulary, keeps the distinct known
context nodes, and considers only existing outgoing association edges from
those nodes. Targets supported by multiple context nodes are aggregated into a
single candidate.

Every returned candidate exposes the inputs used for ranking:

```text
association_score = mean(0.7 * learned_edge_strength + 0.3 * neural_score)
familiarity_score = familiarity * (1 - familiarity_decay), clamped to 0..1
support_score     = supporting_observations / (supporting_observations + 1)
score             = mean(association_score, familiarity_score, support_score)
```

The result also carries the summed supporting edge observations and the number
of distinct context nodes that support the target. Ties are resolved
predictably by association score, familiarity, support count, then token text.
An entirely unknown context yields no candidates. Querying never changes graph
state, so this stage requires no new snapshot block or version bump.

This is intentionally a small primitive. A later planner can combine candidate
sequences, episodic recall, timing, conversational context, and explicit
network policy, but those layers should consume inspectable C23 scores rather
than replace them with hidden prompt logic.

## Persistence

Snapshots are versioned and contain the complete mutable graph, neural
parameters, counters, PRNG state, a mirrored ledger block (snapshot v2), the
episodic-memory block (snapshot v3), and the per-token familiarity block
(snapshot v4). Saving is performed through a temporary file and rename so a
partially written snapshot does not replace the previous state.

Version 1 was host-oriented and wrote fixed-width integers and IEEE-754 floats
directly. Snapshots are not yet a long-term public interchange format; ledger
format version 1 is already byte-order explicit (little-endian) so it can be
migrated independently when needed.

## Growth path

The intended order is:

1. **Language graph** — vocabulary, embeddings, associations, persistence.
2. **Observation ledger** — durable dedupe, replay, provenance, deletion.
   Core ledger, snapshot mirror, and sync-through-ledger are implemented; the
   snapshot rebuild/replay semantics that consume it are part of stage 3+.
3. **Memory** — episodic and semantic structures linked to sources. The first,
   counter-based pass is implemented: source-linked episodes, token summaries,
   recall counters, deterministic eviction, snapshot v3, and a CLI `recall`
   command. Semantic memory is the association graph; richer consolidation is
   future work.
4. **Internal state** — slowly learned preferences/values derived from repeated
   experience, not hard-coded personality text. The first pass is implemented:
   per-token familiarity (exponentially weighted exposure, snapshot v4, CLI
   `familiarity` accessor). Later passes may feed familiarity into recall
   ordering and richer action scoring.
5. **Action model** — candidate generation and inspectable scoring. The first
   pass is implemented as read-only C23 continuation candidates with explicit
   association, familiarity, support, exposure, and context-match evidence.
   Higher-level sequence planning and network-action policy remain future work.
6. **Network behaviour** — carefully rate-limited output through Wolfram.
7. **Long-running runtime** — event-driven or scheduled learning with crash
   recovery and explicit operator controls.

No stage should skip observability just to make the entity appear more human.
