# Author and source interaction state

`atperson` keeps neutral continuity for authors and sources without adding a separate mutable relationship database.

The C23 API in `include/atperson/interaction.h` derives an `atp_interaction_state` on demand from state the core already persists:

- `LEARNED` entries in the graph-side ledger mirror provide encounter count and latest known observation time;
- currently retained episodic memories provide the remembered-episode count;
- a bounded familiarity value is derived mechanically from encounter count as `n / (n + 1)`.

This state is factual exposure, not a social judgement. It does not represent trust, friendship, affinity, sentiment, reputation, or approval. Author lookups are intended to use stable DIDs and source lookups stable AT URIs. The core does not resolve handles or perform network I/O.

## Policy boundary

Only `LEARNED` observations contribute encounters. `SKIPPED` entries remain auditable in the ledger mirror but do not create familiarity, which preserves the ingestion-policy boundary for muted, blocked, unsupported, empty, or moderation-filtered records.

Withdrawal follows the same semantics as the rest of learned state. Withdrawing data does not mutate the current in-memory graph. After `atperson rebuild`, withdrawn ledger entries are absent from the graph mirror and their episodic memories are absent, so they stop contributing to interaction state automatically.

Because the values are derived rather than redundantly stored, snapshot persistence, deterministic replay, ledger compaction, and source withdrawal all share one authority. There is no counter table that can diverge from the experience ledger.

## Query shape

```c
atp_interaction_state state;
atp_status status = atp_graph_interaction_lookup(
    graph,
    ATP_INTERACTION_SUBJECT_AUTHOR,
    "did:plc:example",
    &state);
```

A successful result contains:

- the subject kind and stable identifier;
- `encounter_count`;
- `last_seen_at` (greatest known observation timestamp, or zero when unavailable);
- `remembered_episode_count` for episodes still retained in memory;
- bounded neutral `familiarity` in `[0, 1]`.

Unknown identifiers return `ATP_ERR_NOT_FOUND`. Invalid identifiers or subject kinds return `ATP_ERR_INVALID_ARGUMENT`. Queries are deterministic and read-only.

This is intentionally the minimum author/source state needed before structured planner-context assembly. Direct-interaction counts and richer relationship meaning require explicit conversation/action evidence and belong in later work rather than being inferred from ordinary exposure.
