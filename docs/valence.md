# Experience-derived valence

`atperson` learns a per-token valence score in `[-1, 1]` from explicit experience events only. This is the issue #13 state model: familiarity stays value-neutral, valence is the value-bearing channel, and the two never mix.

## Evidence contract

Only explicit events update valence. Exposure never does — observing a token a thousand times leaves it exactly as neutral as the first sighting.

| Kind | Evidence | Examples |
|------|----------|----------|
| `ATP_VALENCE_ACTION` | Outcome of an action the entity itself took | A post that was received well (positive), one that errored or was rejected (negative) |
| `ATP_VALENCE_INTERACTION` | Direct interaction signal from another actor | Reply, like, mention, block — the caller maps the event to a signal in `[-1, 1]` |
| `ATP_VALENCE_APPROACH` | The entity chose to engage with the subject | Followed a link, replied in a thread |
| `ATP_VALENCE_AVOID` | The entity chose not to engage | Skipped, muted, scrolled past |

The caller decides what real-world event maps to each kind and signal value. The core records the event, folds it into the score, and retains provenance. Nothing is inferred from content, exposure, or developer-authored tables — the empty-start invariant holds: a fresh graph has no valence records at all, and a token observed but never valued reads neutral (`ATP_ERR_NOT_FOUND`, not a fake zero).

An event for a token the entity has never observed is rejected with `ATP_ERR_NOT_FOUND` and mutates nothing: valence attaches to experienced subjects, and interning vocabulary from an event would let one event create learned state.

## Update equation

```
valence' = valence + rate * (signal - valence)
```

- `rate` = `config.valence_rate`, in `(0, 1]`, default `0.25`. Invalid values fall back to the default at graph creation.
- `signal` is clamped to `[-1, 1]`; NaN is rejected.
- The score is an exponential moving average of event signals. An EMA of values in `[-1, 1]` stays in `[-1, 1]` — bounded by construction, no clamping needed on the folded value.
- No time-based decay: valence changes only when events arrive, so the score is exactly the folded evidence. There is no "forgetting" that competes with learning.
- A zero signal is a recorded event that pulls the score toward neutral; it counts in `event_count` but neither `positive_events` nor `negative_events`.

### Reversal and competition

Contrary evidence pulls the EMA across zero. At the default rate, a token valued at `+0.9` reaches `-0.9` after roughly a dozen maximal negative events. The counters make the competition inspectable: `positive_events`, `negative_events`, and `event_count` are the full accounting and never evict.

## Persistence

Valence state persists in snapshot section `ATP_SECTION_VALENCE` (tag 9): the folded per-token records plus the bounded provenance log and eviction counter. The section is optional — pre-#13 snapshots load with empty valence, and readers that do not know tag 9 skip it, so no snapshot format version bump is required.

Loaded state is exact: scores, counters, and the log round-trip byte-for-byte, and future updates continue from the loaded values with the loaded rate.

## Replay semantics

**Defined boundary:** ledger replay reconstructs learned state from the
observation ledger, and the ledger records observations — not valence events.
A rebuilt graph therefore has empty valence *from the ledger alone*. Valence
is experience-derived state whose evidence stream is the action/outcome
journal (#27), not the observation ledger.

`atperson rebuild` replays the journal's explicit valence entries **after**
the ledger, in journal append order, so the rebuilt state includes
experience-derived valence. The journal is the authority for self-authored
experience; the ledger is the authority for third-party observation. Replay
is deterministic: ledger entries in id order, then journal valence entries in
append order.

Before valence affects any autonomous action (issue #13's gating condition),
a valence-event ledger section must exist so rebuilds reconstruct valence
deterministically from the same events. Until then, valence is inspectable
state that does not influence decisions.

## Mapping outcomes to valence (#56)

Valence events enter the system through two explicit operator paths:
`journal apply` (one event, named by hand) and `journal map` (a batch rule
table applied to the recorded journal). Neither runs as a side effect of
`publish`, `sync` or the daemon — the operator decides what experience means.

`journal map <rule-file>` applies an operator-authored rule table to the
journal's recorded outcomes. A rule names a trigger (the action's `outcome`,
optionally a minimum count of later linked events and a window in seconds
after the attempt) and a valence effect (`kind`, `signal`). Rules evaluate in
table order, first match wins per action, and unmapped outcomes produce
nothing. Example: an executed post that received a reply within 24 hours
scores `interaction +0.5`; a denied post scores `action -0.5`.

Two invariants carry over from the core contract:

- **Experienced subjects only.** `map` applies the rule's signal to each
  distinct token of the action's text that already exists in the vocabulary.
  Unknown tokens are skipped, never interned — one mapping run cannot create
  learned state.
- **Idempotency.** Each derived entry is journalled with `provenance`
  `map:<rule-id>` and deduplicated by (source, provenance, token), so running
  `map` twice derives nothing new.

The rule table is text the operator owns; atperson never persists it.
`rebuild` replays the derived valence entries like any other journal valence
entry — the mapping itself is not learned state. See
`docs/action-journal.md` for the rule-table format and failure semantics.

## Provenance

Every event appends to a bounded log (`ATPERSON_VALENCE_EVENT_CAPACITY` = 1024 entries, oldest evicted, evictions counted and exposed via `atp_graph_valence_log_evictions`). The log is queryable newest-first with kind, signal, epoch, token, and source id — enough to explain which observations changed the state recently. The folded counters are the complete accounting; the log is a bounded window, and its eviction counter reports the loss honestly.

## Query surface

```c
/* Record an event (mutating). */
atp_graph_valence_event(graph, "moonlight", ATP_VALENCE_ACTION, 0.8f, epoch, "at://action/1");

/* Inspect one token (read-only; NOT_FOUND when never valued). */
atp_valence_state state;
atp_graph_valence(graph, "moonlight", &state);

/* Enumerate all records in vocabulary order. */
for (size_t i = 0; i < atp_graph_valence_count(graph); ++i) {
    atp_graph_valence_at(graph, i, &state);
}

/* Recent provenance, newest first. */
atp_valence_event log[16];
size_t count = 0;
atp_graph_valence_log(graph, log, 16, &count);
```

`atp_valence_state` carries `token`, `valence`, `event_count`, `positive_events`, `negative_events`, and `last_event_at` — evidence and counters sufficient for inspection without trusting the folded score alone.
