# Episodic recall scoring

`atperson` has two episodic-memory recall surfaces:

- `atp_graph_recall` is the original exact-overlap API retained for compatibility;
- `atp_graph_recall_ranked` in `atperson/memory.h` is the richer inspectable recall path intended for planner context selection and other new work.

The ranked path does not use embeddings-only retrieval, an LLM, or hidden summarisation. An episode becomes eligible only through evidence already present in the C23 learned state: literal summary-token overlap or a direct learned graph association between a query token and an episode-summary token.

## Components

Every `atp_recall_result` contains the full source-linked `atp_episode` plus each score component separately.

`exact_score` is the raw sum of episode-summary weights whose node is present literally in the query.

`association_score` considers summary tokens that did not match exactly. For each such token it finds the strongest direct learned graph edge between that token and any query token, considering either direction. The edge score uses the same inspectable blend as graph association inspection: 70% stored edge strength and 30% current neural pair score. That support is multiplied by the summary-token weight.

`familiarity_score` is a weighted mean over evidence-supported summary tokens. Raw token familiarity is bounded as `f / (f + 1)`, so repeated exposure can provide a secondary boost without growing without limit.

`recency_score` is zero when the observation time or recall time is unknown. Otherwise it is:

```text
1 / (1 + age_seconds / 86400)
```

Future/equal timestamps have zero age rather than negative age.

`use_score` is the bounded transform:

```text
recall_count / (recall_count + 1)
```

The total score is:

```text
exact        * 1.00
+ association * 0.50
+ familiarity * 0.10
+ recency      * 0.10
+ use          * 0.05
```

These constants are public as `ATPERSON_RECALL_*_WEIGHT` in `atperson/memory.h`.

Exact support is intentionally dominant. Association can surface a related episode with no literal overlap, but familiarity, recency and use cannot make an unrelated episode eligible by themselves.

## Determinism and provenance

For a fixed graph and inputs, ranking is deterministic. Ties are resolved by total score, exact support, association support, observation time and finally ledger id, all descending.

The result embeds the original episode, so ledger id, source AT URI, author DID, digest, schema version and timestamps remain inspectable. Ranked recall does not add vocabulary or network state.

Returned episode copies contain recall counters as they existed before the call. As with the original recall API, episodes actually returned to the caller then increment `recall_count` and set `last_recall_at`. A zero-capacity call performs no recall and does not mutate counters.

Because graph edges, token familiarity, episodes and recall counters are already part of the persisted C23 state, ranked recall requires no new snapshot section. The same learned state produces the same score components after snapshot round-trip or deterministic ledger replay.

## Evidence-gated recall

`atp_graph_recall` accepts an optional `atp_recall_config` carrying the recall gate as explicit, operator-tunable policy — not learned state:

| Field | Default | Meaning |
|---|---|---|
| `min_overlap` | `0.0` | Minimum exact-overlap score an episode must reach to be returned |
| `max_prefilter` | `0` (unbounded) | Scan only the most recent N episodes |
| `disable` | `false` | Skip recall entirely |

The default config reproduces the historical eager behaviour byte-identically: any nonzero overlap is eligible, all episodes are scanned.

An optional `atp_recall_report` returns the evidence for the call: how many episodes existed, were scanned, matched with nonzero overlap, survived the gate, and which gate applied (`none`, `disabled`, `prefilter`, `min-overlap`). The prefilter scans the most recent episodes first because episodes are stored in insertion order; the bound is a scan limit, not a relevance filter — ranking within the scanned set is unchanged.

The gate is policy over existing learned state, not new learned state: no vocabulary, familiarity or network mutation occurs, and no new snapshot section is required.

### CLI

```sh
./build/atperson recall "query" [limit] [min-overlap] [max-prefilter] [on|off]
```

The command prints a `recall-report` line naming the gate applied, the scan counts and the effective thresholds, so a reader can see exactly why the returned set is what it is.
