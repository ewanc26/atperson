# Longitudinal self-evaluation

The entity keeps a durable, versioned record of how its own behaviour has moved
over time: what it has actually executed versus attempted, how its pending
social conversations resolve, how publicly visible the replies to its own
outbound actions are, how its explicit valence state drifts and how much
author exposure it picks up per period. A bounded, deterministic pass fills
that surface from the action journal, the learned graph and the observation
ledger. The pass is non-training and operator-mediated: it reads experience
and writes snapshots; it never observes new vocabulary, mutates the graph,
changes the journal, learns or schedules work.

## What a snapshot is

Metric snapshots live in a directory of **one JSON record per file** at
`<data>/metrics/<id>.json` — the same record-per-file convention as thoughts.
Writes are atomic (tmp + rename + directory fsync), a record key is frozen at
pass time, and a snapshot that already exists is refused. The store is locked
separately (`.metrics-lock`) inside the data directory. Each record carries a
`format`/`version` persistence envelope plus the authored metric surface:

```json
{
  "format": "atperson-metrics",
  "version": 1,
  "id": "223mwbzfhpvwm",
  "at": "2026-09-24T19:45:17Z",
  "period_start": "2026-09-17T19:45:17Z",
  "period_end": "2026-09-24T19:45:17Z",
  "cadence_seconds": 604800,
  "previous": { "id": "223m8hgzqf3rm", "at": "2026-09-17T19:45:17Z" },
  "metrics": {
    "actions": {
      "attempts": 12, "executed": 9, "denied": 1, "deferred": 1,
      "failed": 1, "dry_run": 0, "success_rate": 750
    },
    "interaction": {
      "invites": 2, "invites_replied": 1, "invites_expired": 1,
      "invites_pending": 0, "success_rate": 500
    },
    "reply_ratio": { "events": 40, "replies": 23, "ratio": 575 },
    "valence": { "updates": 14, "tokens": 9, "drift": -250 },
    "familiarity": { "authors": 31, "encounters": 212, "new_authors": 6, "accretion": 194 }
  },
  "trace": {
    "episodes": 84,
    "events": 15,
    "resolutions": 3,
    "valence_updates": 7,
    "new_authors": ["did:plc:aaaa", "did:plc:bbbb"],
    "top_valence": [ { "token": "bpd", "kind": "approach", "sum": 750, "count": 2 } ]
  }
}
```

The `metrics` block is cumulative — it measures the entity since start, not the
week. The `trace` block bounds the per-period activity that moved the
cumulative numbers, so a snapshot stays explainable and the period it covered
is visible. `previous` points at the immediately preceding snapshot in the
series; trend deltas are derived at display time from the stored sequence
rather than stored as a second representation.

Ratios and signed sums (`success_rate`, `ratio`, `accretion`, `drift`, `sum`)
are stored as per-mille integers — the lexicon spec has no fractional type, so
the record publishes with `integer` fields scaled ×1000. The store keeps
doubles in memory and converts at the JSON boundary.

## Running it

```sh
atperson selfeval              # run the pass now and report whether it wrote
atperson metrics               # newest first, limit 50, with trend deltas
atperson metrics 5 --since 2026-09-01T00:00:00Z
atperson daemon                # runs the pass each cycle when enabled
```

`selfeval` is an explicit operator request and runs regardless of
configuration. The daemon hook runs the pass only when `ATPERSON_SELFEVAL=1`.
A run inside the cadence writes nothing.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATPERSON_SELFEVAL` | off | Enable the daemon's per-cycle self-eval step; `selfeval` itself ignores it |
| `ATPERSON_SELFEVAL_CADENCE_SECONDS` | 604800 | Minimum gap between snapshots (bounded [1, 366 days]) |
| `ATPERSON_SELFEVAL_MAX_TRACE` | 8 | Max per-period new-author DIDs kept in the trace (bounded [1, 512]) |
| `ATPERSON_SELFEVAL_MAX_TRACE_GROUPS` | 8 | Max per-period `top_valence` groups kept in the trace (bounded [1, 64]) |

## The pass

For the cadence-long window ending at `now`, the pass:

1. **Checks the cadence.** The first snapshot is always due; a later run is
   due only when the previous snapshot is at least `CADENCE_SECONDS` old. A
   backwards clock keeps the snapshot series safe rather than forcing writes.
2. **Computes cumulative metrics** from the journal and graph: action outcomes
   across all attempts; per-thread terminal intent state (#150); the reply
   share of events linked to the entity's own actions; the net valence signal
   over all explicit updates (#13); and author familiarity growth from the
   ledger's `LEARNED` entries.
3. **Stamps a bounded trace** for the window: episodes, linked events,
   resolutions, valence updates, a suffix of new authors and the strongest
   (token, kind) valence movement groups.
4. **Writes exactly one snapshot** when due, and nothing otherwise.

The `selfeval` command reports the reason it wrote (or skipped):
`no prior snapshot`, `cadence elapsed` or `inside cadence`, followed by a
compact summary line of the same numbers the CLI listing shows.

## Deltas and listing

`metrics` prints one record per entry, newest first. Each entry shows its
`at`/`span`/`cadence` (and `previous` id, or `first-snapshot` on the first),
then the cumulative `actions admitted/executed/denied/deferred/failed/dry-run
success`, `interaction invites/replied/expired/pending success`, `reply_ratio`
as a percentage over linked events, `valence drift` over updates/tokens,
`familiarity authors (+new this period, accretion) encounters`, and the `trace`
window plus new-author and `top_valence` detail. When the snapshot has a
predecessor, the entry also prints its trend deltas against it: `delta exec`,
`delta success`, `delta drift`, `delta authors`, and signed percentage moves
for interaction success and reply ratio. Signed deltas use a `+`/`-` prefix
with two decimals; counters are integer diffs. The first snapshot has no
predecessor, so deltas are omitted. The header echoes the persisted format and
version plus the number of snapshots shown:

```text
metrics: format=atperson-metrics version=1 snapshots=2
```

`--since <iso>` filters to snapshots at or after an RFC 3339 instant, and the
optional positional limits the number of records shown (default 50, clamped to
1000).

## Durability and repair

- A missing directory means an empty store.
- Writes are atomic and immutable: a record that already exists is refused, a
  crash leaves at most an ignored `<id>.json.tmp` leftover, and a record whose
  `id` disagrees with its filename fails loudly rather than silently
  reindexing.
- A malformed record fails loudly with a `MetricError`; unsupported formats and
  versions are refused. Files that are not `<id>.json` records are ignored by
  the reader.
- Records are ordered by record key, which encodes creation time, so reads
  reproduce series order deterministically.
- Snapshots are durable runtime output but not learned state: they are never
  replayed by `rebuild` and train nothing.

## Network boundary

Self-evaluation performs no network I/O, reads no credentials, and can never
schedule, approve, publish or gate an action. It is a read-only observer over
the journal and graph that records what it observed and reports back to the
operator.
