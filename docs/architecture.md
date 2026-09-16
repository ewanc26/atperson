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
- future action selection and publishing policy.

`LanguageGraph` is intentionally a thin RAII wrapper over the C API; the
`Ledger` wrapper is the same for the C23 observation ledger.

## Wolfram boundary

AT Protocol networking belongs to
[`ewanc26/wolfram`](https://github.com/ewanc26/wolfram).

atperson consumes Wolfram's C23 implementation and C++ RAII ownership layer. It
must not grow parallel implementations of XRPC, session management, DID/handle
resolution, repository operations, or Bluesky procedures.

The first network path uses `wf_agent_login` and `wf_agent_get_timeline`, then
extracts public post text and passes each post to the learning core with its AT
URI as the source identifier.

## Observation ledger

The ledger is a durable, append-only C23 log of every observation fed to the
learning core. It records, per observation:

- AT URI / stable source identifier;
- author DID where applicable;
- observed-at time;
- content digest;
- model/schema version;
- processing outcome.

The log is the authority for what the entity has seen. A committed observation
(`LEARNED` or `SKIPPED`) is never trained on again across process restarts;
outcome changes append a small patch record rather than rewriting log bytes.

### Files and layout

- `path` is the record log: `"ATPLDG01"` then a little-endian version u32.
- Each record is `len u32 | crc u32 (FNV-1a 32) | type u8 | payload`, where
  type 1 is an observation entry and type 2 is an outcome patch.
- `<path>.off` is the durable commit marker: `"ATPLOF01"`, version, committed
  count, and the byte offset of the committed prefix (28 bytes total).

Multi-octet integers are little-endian, a deliberate format decision: snapshot
v1 stays host-oriented, and the two files may migrate independently in a future
version bump.

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
PENDING)` -> observe -> `set_outcome(id, LEARNED)` (or `SKIPPED` for empty
text), then mirror the entry into the graph snapshot (v2) so recent provenance
is inspectable in one file. The ledger remains authoritative for rebuilds.

Deleting a learned contribution is still an open problem; the preferred model
remains rebuild-from-ledger rather than approximate inverse gradient steps.

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
PENDING)` -> `remember(...)` -> `set_outcome(id, LEARNED)` (or `SKIPPED` for
empty text), then the entry is mirrored into the graph snapshot (v2) and the
episode (if selected) is stored in memory (v3). The ledger remains
authoritative for rebuilds; memory is linked to it by ledger id.

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
as the snapshot v4 familiarity block. This score is intended to later inform
recall ordering and action scoring as a purely behavioural signal.

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
   ordering and action scoring.
5. **Action model** — candidate generation and inspectable scoring.
6. **Network behaviour** — carefully rate-limited output through Wolfram.
7. **Long-running runtime** — event-driven or scheduled learning with crash
   recovery and explicit operator controls.

No stage should skip observability just to make the entity appear more human.
