# Experience-derived drives (#148)

Two bounded, deterministic, fully inspectable initiation signals that let the
scheduler prefer which recent observations to decide on first. They are
computed purely from existing learned evidence — the language graph and the
action journal — and they never change *whether* the entity may act, only
*which* candidate it examines first. They exist to replace a frozen
newest-first order with a partially-known-preferring, reciprocity-aware one as
issues #148–#153 build out the roadmap's autonomous behaviour surface.

## Contract

- **Curiosity = novelty × adjacency.**
  - *Topic*: tokenise the candidate payload; `novelty = 1/(1 + mean
    familiarity)` over the known tokens and `adjacency = known/total`. A fresh
    graph has no known tokens (`0`); a heavily-exposed message has novelty
    decaying toward zero; only a partially-known subject lands strictly
    between. Novelty uses the same bounded `f/(f+1)` form as author and
    interaction familiarity.
  - *Author*: `novelty = 1/(1 + encounters)`, `adjacency = 1` iff the author
    has remembered episodes (the encounter count is derived from episodes,
    never a second counter store).
  - Final curiosity is `max(topic, author)`, clamped to `[0, 1]`. An empty
    graph and an empty journal yield all zeros: a pristine entity initiates
    nothing on its own.
- **Reciprocity** requires the entity to have acted, and a public record to
  have referenced that action (see `docs/action-journal.md` linkage).
  - `event` (strength 1.0): the candidate observation *is* the referencing
    record — its `source_id` equals the journal event URI.
  - `author` (strength 0.5): the candidate's author referenced the entity's
    action within the bounded window (default 7 days). Timestamps that fail to
    parse are treated as outside the window.
  - Otherwise `none` (`0`). No action link, no reciprocity — exposure alone
    never becomes a drive.
- **Ordering**: reciprocity descending, then curiosity descending, then the
  original newest-first order, via `std::stable_sort`. Deterministic for
  fixed inputs.
- Drives reorder candidate contexts *only*. They never weaken the C23
  decision gates, never skip the proposal/approval/gate chain, and never
  widen `max_contexts`/`max_proposals`/`max_executions`.

## Boundaries

All drive computation lives in `src/app/scheduler/drives.{hpp,cpp}` (C++23,
no network, no Wolfram, no clock — `now` is injected). It reads the C23 graph
(`has_token`, `familiarity`, `episodes`) and the journal store but writes
nothing: no learned-state mutation, no ledger entry, no journal record. This
is deliberate — a "no second model" rule. The C23 core remains the authority
for what is known; the drive signals are derived, bounded, operator-inspectable
orderings on top of it.

## Scheduler wiring

`SchedulerConfig.drives_enabled` (off by default) is set from
`ATPERSON_SCHEDULER_DRIVES=1`. When enabled, the cycle selects the same
candidates (newest committed, payload-bearing ledger entries, bounded by
`max_contexts`) and orders them before decision. The report exposes
`ordered_by_drives`; the daemon prints `(drive-ordered)` when it fires.

## Inspection

`atperson drives [max-contexts]` is read-only: it prints the candidate
contexts in scheduler-preference order, each with its curiosity, reciprocity,
how reciprocity was earned (`none`/`event`/`author`), author DID and source
URI, plus a reminder of the scheduler gate state.

A fresh entity shows `curiosity=0.000 reciprocity=0.000 via=none`. After the
entity acts and a thread replies, the exact reply observation earns
`reciprocity=1.000 via=event`; other posts by the same author within the
window earn `reciprocity=0.500 via=author`.