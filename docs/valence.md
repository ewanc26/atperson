# Experience-derived valence

`atperson` keeps familiarity and valence separate on purpose. Familiarity answers "how much exposure has this token had?"; valence is the value-bearing state learned from explicit experience events.

A token can be extremely familiar and remain completely neutral.

## Evidence contract

Valence is a per-token score in `[-1, 1]`. Exposure never updates it. Only an explicit event can do that.

| Kind | Evidence |
| --- | --- |
| `ATP_VALENCE_ACTION` | Outcome of an action the entity took |
| `ATP_VALENCE_INTERACTION` | Direct interaction evidence from another actor |
| `ATP_VALENCE_APPROACH` | The entity chose to engage with the subject |
| `ATP_VALENCE_AVOID` | The entity chose not to engage with the subject |

The caller maps a real event to a kind and signal. The core does not infer value from content, repetition or a developer-authored personality table.

A valence event for an unknown token returns `ATP_ERR_NOT_FOUND` and mutates nothing. Value can attach to something the entity has experienced; it cannot create vocabulary on its own.

## Update rule

```text
valence' = valence + rate * (signal - valence)
```

`signal` is clamped to `[-1, 1]`; NaN is rejected. `rate` comes from `config.valence_rate`, defaults to `0.25` and must be in `(0, 1]`.

The value is an exponential moving average of event signals. There is no time-based decay. A zero signal is still an event and pulls the value towards neutral, but it does not increment either the positive or negative event counter.

Contrary evidence can move a value back through zero. The folded score is therefore never the only evidence exposed: each state also keeps `event_count`, `positive_events`, `negative_events` and the last event time.

## Persistence and replay

Snapshot section `ATP_SECTION_VALENCE` stores the folded valence records and bounded recent provenance log. Older snapshots without the section load with empty valence state.

Snapshots are not the only place valence lives. Explicit valence events are also recorded in the [`action-journal.md`](action-journal.md), and `atperson rebuild` replays them after rebuilding observation-derived state from the ledger:

1. ledger observations replay in id order;
2. journal valence events replay in append order.

This is the durable boundary: the observation ledger is authoritative for third-party observations, while the journal carries the entity's own recorded experience and applied value updates.

Valence is currently inspectable state; it does not by itself grant permission for an outbound action. Network permission still belongs to the runtime policy/control path.

## Mapping outcomes to valence

There are two explicit operator paths:

- `journal apply` records one named valence event;
- `journal map` applies an operator-authored mapping table to recorded action outcomes.

Neither runs automatically from `publish`, `sync` or the daemon.

A mapping rule can match an action outcome and optionally later linked events, then provide a valence kind and signal. Rules run in order and the first match wins. Unknown tokens are skipped rather than interned, and derived events are idempotent by source, mapping provenance and token.

The rule file itself is operator configuration, not learned state and not persisted by `atperson`. See [`action-journal.md`](action-journal.md) for the format.

## Provenance

Every applied event enters a bounded recent provenance log. `ATPERSON_VALENCE_EVENT_CAPACITY` is currently 1024 entries; the oldest entries are evicted when the log fills and the eviction count is exposed explicitly.

The bounded log is for explanation, not complete accounting. The per-token counters are the durable totals.

## API

```c
atp_graph_valence_event(
    graph,
    "moonlight",
    ATP_VALENCE_ACTION,
    0.8f,
    epoch,
    "at://action/1");

atp_valence_state state;
atp_graph_valence(graph, "moonlight", &state);

for (size_t i = 0; i < atp_graph_valence_count(graph); ++i) {
    atp_graph_valence_at(graph, i, &state);
}

atp_valence_event log[16];
size_t count = 0;
atp_graph_valence_log(graph, log, 16, &count);
```

A token with no valence evidence returns `ATP_ERR_NOT_FOUND`; the API does not invent a fake neutral record.