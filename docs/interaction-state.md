# Author and source interaction state

`atperson` derives neutral continuity for authors and sources from evidence it already stores. It does not maintain a second mutable relationship database beside the ledger and episodic memory.

The C23 API in `include/atperson/interaction.h` derives an `atp_interaction_state` from:

- `LEARNED` ledger-mirror entries for encounter count and latest observation time;
- currently retained episodic memories for remembered-episode count;
- bounded familiarity `n / (n + 1)` derived from encounter count.

This is exposure, not a judgement. It does not mean trust, friendship, affinity, reputation, sentiment or approval.

Author identifiers should be stable DIDs and source identifiers stable AT URIs. The core does not resolve handles or perform network I/O.

## Policy boundary

Only observations with outcome `LEARNED` contribute encounters. Skipped records remain auditable but do not create familiarity, preserving the ingestion-policy boundary for muted, blocked, unsupported, empty or moderation-filtered material.

Withdrawal follows the same rebuild semantics as the rest of learned state. After a rebuild, withdrawn entries and their retained episodes no longer contribute.

Because the values are derived rather than redundantly stored, replay, compaction and withdrawal all share the same authority. There is no relationship counter table that can drift away from the evidence it supposedly represents.

## API

```c
atp_interaction_state state;
atp_status status = atp_graph_interaction_lookup(
    graph,
    ATP_INTERACTION_SUBJECT_AUTHOR,
    "did:plc:example",
    &state);
```

A successful result includes:

- subject kind and stable identifier;
- `encounter_count`;
- `last_seen_at` (or zero when unknown);
- `remembered_episode_count`;
- neutral bounded `familiarity`.

Unknown identifiers return `ATP_ERR_NOT_FOUND`; malformed identifiers or subject kinds return `ATP_ERR_INVALID_ARGUMENT`. Queries are deterministic and read-only.

Richer relationship meaning needs richer evidence. It should not be inferred from ordinary exposure just because the project has an author identifier available.