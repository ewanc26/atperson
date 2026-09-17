# Action/outcome journal (`atperson journal`)

The action/outcome journal (#27) is the durable, replayable record of the
entity's **own outbound actions** and what happened to them afterwards. It is
the authority for *self-authored* experience, complementing the observation
ledger, which is the authority for *third-party* experience.

## Why a separate journal

The observation ledger records every observation fed to the learning core and
is the authority for cross-run deduplication and replay. It records what the
entity *saw*. It does not record what the entity *did*.

The journal records, for every outbound attempt:

- the exact action (rkey, text, approval digest);
- the runtime's execution outcome (`executed`, `denied`, `deferred`,
  `failed`, `dry_run`);
- the network result (at-URI, CID) when executed;
- later — the public replies/quotes that reference the executed record.

Executed-action provenance stays separate from ordinary observations while
remaining auditable and reconstructable.

## Overlap with the outbound audit log

The journal deliberately overlaps the #25 outbound audit log: both record
every attempt. They serve different contracts:

- the **audit log** is the operator-facing operational record of execution
  (`~/.ewanc26/atperson/outbound-audit.jsonl`, `ATPERSON_OUTBOUND_AUDIT`);
- the **journal** is experience provenance that events and valence entries
  hang off, and it is the replay source for explicit state updates.

Neither is derived from the other.

## Storage

Append-only JSONL at `<data>/action-journal.jsonl`
(`ATPERSON_ACTION_JOURNAL`). One compact JSON object per line, fsync after
every append. A crash mid-append leaves at most one torn final line; loading
reports and truncates it rather than silently reinterpreting partial bytes.

Cross-process mutations serialise under the caller's lock (the daemon holds
the writer lock; `publish` and the valence command hold the outbound lock),
so the store itself performs no locking.

## Entry kinds

### `action`

One attempted outbound action. `id` is the frozen rkey from the #25 action
document: stable up front, idempotent under putRecord retry, and the tail of
the executed record's at-URI, so events link to actions by identifier rather
than by matching mutable text. `digest` is the #22 approval digest (16
lowercase hex). `uri`/`cid` are populated only when `Executed`.

### `event`

How a later public record referenced an executed action. `via` names the
referencing field: `parent` (a direct reply), `root` (a reply in the same
thread)
or `quote` (a quote post). Events link to actions by `action_id`, not by text.

### `valence`

One explicit experience-derived state update the operator applied (#13). The
journal stores what was applied so `rebuild` can replay it after the ledger; it
never applies anything itself. `kind` is a valence kind name
(`action`, `interaction`, `approach`, `avoid`), `signal` the clamped `[-1, 1]`
value, `source` the journal action id or AT URI the event cites.

## The `journal` command

```sh
atperson journal actions [limit]        # attempted actions (newest last)
atperson journal events [limit]         # linked outcome events
atperson journal valence [limit]        # applied valence updates
atperson journal apply <token> <kind> <signal> <source-id>
```

The listing subcommands are read-only and run lock-free, like other
inspection commands. They render the journal's append order with stable field
widths.

### `journal apply`

`apply` is the only mutating path besides `rebuild` that turns journal
experience into valence state. It is opt-in by construction: the operator names
the token, the valence kind, the signal and the source (a journal action id or
an AT URI). Nothing here derives valence from the journal automatically — the
journal records experience; the operator decides what it means.

`apply` takes the data-directory writer lock (the same lock as ingest/rebuild)
across the graph mutation, the journal append and the model save, so the three
durable effects commit as one unit or not at all. The valence API throws on
unknown tokens (valence attaches to experienced subjects only); the command
surface returns 2 for bad arguments and throws for I/O.

## Replay semantics

`atperson rebuild` now replays the journal's explicit valence entries **after**
the observation ledger, so experience-derived valence state survives a rebuild
rather than being lost. The journal is the authority for self-authored
experience; the ledger is the authority for third-party observation.

Replay order is deterministic: ledger entries in id order, then journal
valence entries in append order. The rebuilt output reports both counts:

```text
replayed N observation(s) from the ledger (mirrored M skipped, excluded …)
replayed V valence event(s) from the journal
```

This is the defined boundary that `docs/valence.md` previously described as a
gap: valence is now reconstructable from the same durable evidence stream as
the rest of learned state.

## Failure semantics

- Malformed entries, unsupported versions and impossible field combinations
  throw `JournalError`.
- A missing file yields an empty journal (the journal starts empty).
- A torn final line is truncated and reported through
  `JournalContents::repaired_torn_tail`.
- Unknown `type` values throw: the journal never skips content it does not
  understand.

## Locking

`journal apply` is a state-mutating command and takes the exclusive writer
lock on the data directory, alongside `ingest`, `ingest-file`, `sync`, `cursor
reset`, `rebuild`, `compact`, `withdraw`, `daemon` and `compact`. The listing
subcommands run lock-free like other read-only commands.

## Network and safety boundary

The journal records outbound experience. It does not itself perform any
network I/O, and nothing here feeds learning directly: valence is applied only
through the explicit C23 API by the operator-facing `apply` command. The
network path remains read-only for ingestion; autonomous posts, replies,
likes, follows, reposts, DMs and moderation actions are not added as a side
effect of learning, memory or planning work.