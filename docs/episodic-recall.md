# Episodic recall scoring

`atperson` keeps two episodic recall surfaces:

- `atp_graph_recall` — the original exact-overlap API kept for compatibility;
- `atp_graph_recall_ranked` — the richer evidence-bearing path used by newer planner/context work.

Ranked recall does not hide retrieval behind embeddings or an LLM. An episode first needs evidence already present in C23 learned state: exact summary-token overlap or a direct learned graph association between the query and the episode summary.

## Score components

Every `atp_recall_result` includes the source-linked episode plus the individual components used to rank it.

`exact_score` is the sum of matching summary-token weights.

`association_score` considers summary tokens that did not match exactly. For each one it finds the strongest direct graph edge to a query token in either direction, using the same 70% stored-edge / 30% neural-pair blend as normal association inspection, then weights that support by the summary weight.

`familiarity_score` is a weighted mean over evidence-supported summary tokens using bounded familiarity `f / (f + 1)`.

`recency_score` is:

```text
1 / (1 + age_seconds / 86400)
```

when timestamps are known, otherwise zero.

`use_score` is:

```text
recall_count / (recall_count + 1)
```

The final score is:

```text
exact        * 1.00
+ association * 0.50
+ familiarity * 0.10
+ recency      * 0.10
+ use          * 0.05
```

The constants are public as `ATPERSON_RECALL_*_WEIGHT` in `atperson/memory.h`.

Exact/association evidence decides eligibility. Familiarity, recency and previous use can refine ranking but cannot make an unrelated episode relevant by themselves.

## Determinism and provenance

For fixed learned state and inputs, ordering is deterministic. Ties resolve by total score, exact support, association support, observation time and ledger id, all descending.

The returned episode retains its ledger/source/author provenance. Ranked recall itself adds no vocabulary or network state.

Normal ranked recall records use: returned episodes increment `recall_count` and update `last_recall_at`. A zero-capacity call performs no recall. Planner context uses the separate preview path so considering a memory does not count as actually recalling it.

## Evidence gate for exact recall

`atp_graph_recall` accepts optional `atp_recall_config` policy:

| Field | Default | Meaning |
| --- | --- | --- |
| `min_overlap` | `0.0` | Minimum exact-overlap score |
| `max_prefilter` | `0` | Maximum recent episodes scanned (`0` means unbounded) |
| `disable` | `false` | Disable recall for the call |

The default reproduces the historical eager behaviour.

An optional `atp_recall_report` exposes how many episodes existed, were scanned, matched, passed the gate and which gate was active. The recent prefilter is only a bounded scan window; it does not rewrite relevance ranking within that window.

These controls are call-time policy over existing learned state, not additional learned state.

## CLI

```sh
./build/atperson recall "query" [limit] [min-overlap] [max-prefilter] [on|off]
```

The command prints the effective gate and scan counts so it is possible to see why a result set was returned rather than treating recall as an opaque search.