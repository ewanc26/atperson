# Network-native state (#142)

The entity's durable experience — the observation ledger and the action
journal — is local-first. #142 makes it reconstructable from the network:
the publisher drains committed local state to AT Protocol records under
the entity's own DID, and the reconstruct path rebuilds fresh state from
those records. A new host with no local data can recover the entity's
experience from its DID alone.

The local ledger stays the commit authority. A network outage never
blocks or corrupts a local commit; the publication backlog drains on
reconnect.

---

## Collections

All records live under the `click.croft.atperson.*` NSID authority, in
the entity's own repository:

| Collection | Content | rkey |
|---|---|---|
| `click.croft.atperson.observation` | provenance for one ledger entry | base-32 of the ledger id |
| `click.croft.atperson.action` | one self-authored outbound action | the frozen action id |
| `click.croft.atperson.valence` | one self-authored valence application | base-32 of (at_epoch, ordinal) |
| `click.croft.atperson.thought` | one self-authored internal note | the frozen thought id |

The collections have no published lexicons; records are written with
`validate=0` and validated locally before every write. Record shapes are
versioned (`format: "atperson-record"`, `version: 1`); a format change
that alters what a rebuild learns requires a version bump.

## What is published — and what is not

**Observation records carry provenance, never content.** Third-party
post text is not republished. The record holds the source URI, author
DID, timestamps, the 64-bit content digest and the conversational
context (reply root/parent, quote target). Reconstruction re-fetches
the bytes from the source URI and verifies the digest before replaying.

u64 fields (id, digests, epochs) are serialised as decimal strings — a
u64 does not survive a JSON number round-trip.

**Thoughts ARE published in full.** A thought is the entity's own words,
not third-party content, so the record carries the complete text — no
digest-and-refetch. `atperson thought <text>` records one locally
(`reflection` is the only kind); the next drain publishes it, and
reconstruct replays the text directly. Thoughts are durable notes the
entity writes for its future self; they are not trained on
automatically.

**Journal events are not published.** They reference third-party
records and exist for local inspection only.

**Learned model bytes are not published.** The model is rebuildable by
replaying the ledger, which the records make possible; publishing
weights would duplicate that with a larger surface.

**Credentials are never published.** Session material is host-local.

## Publisher

`atperson statepub drain` performs one bounded pass:

1. **Withdrawal propagation** — rechecks a bounded window of already
   published entries for outcome changes. A withdrawn entry republishes
   its observation record with `outcome: "withdrawn"` (same rkey,
   putRecord update), so a network rebuild excludes the experience.
2. **New observations** — committed entries (LEARNED/SKIPPED/FAILED)
   from the cursor forward. PENDING entries are skipped; the next drain
   picks them up once committed.
3. **Journal entries** — actions and valence, in append order, counted
   cursors (the journal is append-only, so counts are stable).
4. **Thoughts** — the local thought store, in append order.

Progress is checkpointed in `<data>/replicate/cursor.json`, saved
atomically (stage + rename) after each confirmed write. A network
failure stops the drain at the last confirmed write; the backlog stays
local and drains on the next pass. putRecord is idempotent on
(collection, rkey), so retried overlaps are safe.

`max_records_per_drain` (default 50) caps writes per pass — PDS writes
are rate-limited, and a large backfill chunks across passes.

`atperson statepub status` reports cursor position against ledger and
journal counts without touching the network.

### Offline mode

`atperson statepub drain --offline` runs the same bounded drain with the
same gates and cursor, but stages the exact record JSON to
`<data>/replicate/records/<collection>/<rkey>.json` instead of the
network — one directory per collection, one file per rkey, mirroring the
repository layout. The staged bytes are identical to what an online drain
would publish; writes are atomic and fsynced.

Offline and online passes share the cursor, so they interleave freely:
drain offline while disconnected, and a later online drain resumes from
the same point (putRecord is idempotent on (collection, rkey), so any
overlap is safe).

### Gates

- **Control write gate.** `statepub drain` is an outbound network write
  procedure: the master `writes_enabled` gate applies, fail-closed.
- **Headroom.** The runtime resource preflight applies as to any write.
- **Lock.** A dedicated statepub lockfile serialises drains; it does not
  compete with the daemon's long-held writer lock.

## Reconstruct

`atperson reconstruct --into <dir>` rebuilds fresh state:

1. Lists observation records, parses and sorts them by ledger id.
2. For each: skips withdrawn records, re-fetches content from the
   source URI, verifies the digest, appends to a fresh ledger with the
   recorded outcome and context.
3. Replays action and valence records to a fresh journal, and thought
   records to a fresh thought store.

Fail-closed throughout: a source that is unavailable, or content whose
digest does not match the record, is reported and NOT replayed. A
tampered or hallucinated record cannot enter the ledger. The target
directory must not already contain state — reconstruct never merges
into existing state.

After reconstruct, `atperson rebuild` replays the recovered ledger
through the normal path to retrain the model.

## Test coverage

`tests/replicate/replicate.cpp` (offline, fake writer and record source):

- Record round-trips for all four kinds, plus corruption rejection.
- The full acceptance path: train → drain → fresh directory →
  reconstruct → rebuilt ledger matches (entries, outcomes, payloads,
  withdrawal excluded).
- Network failure mid-drain retains the backlog; a retry publishes.
- Digest mismatch fails closed.
- Offline drain stages record files under `records/<collection>/`, and
  a second pass is a no-op.
