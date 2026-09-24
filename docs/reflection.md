# Deterministic reflection pass

The entity keeps a durable, inspectable surface of what it has noticed about its
own experience. A bounded, deterministic pass fills that surface from the
action journal, the learned graph and the existing thought store. The pass is
non-training: it reads experience-derived state and writes thoughts; it never
observes new vocabulary, mutates the graph, changes the journal or learns.

## What a thought is

AT Protocol is "just JSON", and a record is one JSON object. The thought
store follows that shape: thoughts live in a directory of **one JSON record
per file** at `<data>/thoughts/<id>.json`. There are no delimiters to
interpolate — each file is exactly one record. Writes are atomic
(tmp + rename + directory fsync), and a record key is frozen at creation, so a
crashed write can never corrupt an earlier thought. The store is locked
separately (`.thoughts-lock`) inside the data directory.

```json
{
  "format": "atperson-thought",
  "version": 1,
  "id": "223mwbzfhpvwm",
  "kind": "reflection",
  "text": "the wolf howls at a moon that never changes",
  "about": null,
  "at": "2026-09-24T19:45:17Z",
  "span_start": "2026-09-17T19:45:17Z",
  "span_end": "2026-09-24T19:45:17Z",
  "topic": "consolidation"
}
```

There are three kinds:

- **`reflection`** — a thought about a specific moment; this is what
  `atperson thought <text...>` records.
- **`consolidation`** — a periodic summary of one window of experience.
- **`movement`** — a flagged shift (valence, unfamiliar authors, reply ratio).

`span_start`/`span_end`/`topic` are carried by pass-written thoughts and omitted
from hand-written reflections. The format version stays 1 for both shapes.

## Running it

```sh
atperson thought "the moon pulls the tide"
atperson thoughts              # newest first, limit 50
atperson thoughts 5 --since 2026-09-01T00:00:00Z --kind movement
atperson reflect               # run the pass now and report what it wrote
atperson daemon                # runs the pass each cycle when enabled
```

`thought` records and takes the store lock. `thoughts` is read-only. `reflect`
is an explicit operator request and runs regardless of configuration. The
daemon hook runs the pass only when `ATPERSON_REFLECTION=1`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATPERSON_REFLECTION` | off | Enable the daemon's per-cycle reflect step; `reflect` itself ignores it |
| `ATPERSON_REFLECTION_CADENCE_SECONDS` | 86400 | Consolidation cadence |
| `ATPERSON_REFLECTION_MAX_THOUGHTS` | 8 | Max thoughts written per pass (bounded [1, 64]) |
| `ATPERSON_REFLECTION_WINDOW_SECONDS` | 604800 | Trailing observation window |
| `ATPERSON_REFLECTION_VALENCE_DELTA_MIN` | 0.25 | Valence movement magnitude that triggers |
| `ATPERSON_REFLECTION_UNFAMILIAR_MIN` | 2 | Unfamiliar authors in the window that trigger |
| `ATPERSON_REFLECTION_REPLY_SHIFT_MIN` | 0.25 | Reply-ratio shift between windows that triggers |

## The pass

For the window ending at `now`, the pass:

1. **Evaluates triggers** against the trailing window and the previous window.
2. **Stamps everything deterministically**: each pass identifies its window by
   `now`, so identical inputs and clock produce identical thoughts.
3. **Bounds the write** to `ATPERSON_REFLECTION_MAX_THOUGHTS` per pass.

### Movement triggers

- **Valence movement.** Journal valence entries in the window are grouped by
  (token, kind); a group whose net movement meets
  `VALENCE_DELTA_MIN` triggers a `movement` thought.
- **Unfamiliar authors.** Episode authors in the window whose learned-exposure
  ledger shows at most one encounter (`LEARNED` ledger outcomes for that author)
  trigger when the count meets `UNFAMILIAR_MIN`.
- **Reply-ratio shift.** The share of journal events arriving `via` a reply is
  compared between the previous and trailing windows; a shift of at least
  `REPLY_SHIFT_MIN` triggers.

### Consolidation

A consolidation is written when the cadence is due (no prior consolidation, or
the last one is older than `CADENCE_SECONDS`). It summarises the window's
valence updates, episodes, authors, linked events and resolutions with the same
dropped-field-empty rule as the movements.

### Bounded and deterministic

The total write is capped per pass, with movement triggers served first and
consolidation next while budget remains. Two determinism rules keep a window
single-shot:

- a trigger that already produced a thought for its exact topic and window
  never fires again; and
- the whole movement phase is skipped once the window already has any movement
  thought entered. A rerun over the same inputs and clock therefore writes
  nothing, and the write set of two passes over identical state is identical.

## Durability and repair

- A missing directory means an empty store.
- Writes are atomic and immutable: a record that already exists is refused, a
  crash leaves at most an ignored `<id>.json.tmp` leftover, and a record whose
  `id` disagrees with its filename fails loudly rather than silently
  reindexing.
- A malformed record fails loudly with a `ThoughtError`; unsupported formats
  and versions are refused. Files that are not `<id>.json` records are ignored
  by the reader.
- Records are ordered by record key, which encodes creation time, so reads
  reproduce write order deterministically.
- Thoughts are durable experience but not learned state: they are never replayed
  by `rebuild` and never train anything.

## CLI semantics

- `thoughts [limit] [--since <iso>] [--kind <k>]`; the default limit is 50 and
  is clamped to 1000. `--since` filters to entries at or after an RFC 3339
  instant; `--kind` filters to one kind.
- `thought` joins its text parts with spaces, refuses empty text, stamps the
  current RFC 3339 time and prints the recorded id.
- `reflect` reports what it wrote (or deduplicated/ran/skipped) plus the window
  summary counts. It is read-only over the journal and graph, so it is safe in
  every schedule.

## Network boundary

Reflection performs no network I/O and reads no credentials. Autonomous posts,
replies, likes, follows, reposts, DMs and moderation are never created as side
effects of the pass.