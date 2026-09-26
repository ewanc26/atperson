# Remote operator control (#143)

A headless host has no shell to run `atperson control pause` on. This is the
channel that replaces one: the operator publishes a record to their own AT
Protocol repo, and the daemon applies it to local control state on its next
cycle.

It is deliberately narrow. The channel can only do what
`atperson control` already does locally — it is the same switch set, reached
over the network, not a second and wider control surface.

## The record

One collection, under the **operator's** DID, never the entity's:

```
click.croft.atperson.control#request
```

```json
{
  "format": "atperson-control-request",
  "version": 1,
  "type": "click.croft.atperson.control#request",
  "seq": "7",
  "op": "pause",
  "at": "2026-09-26T12:00:00Z"
}
```

| field | meaning |
| --- | --- |
| `seq` | decimal string, strictly increasing per operator. The first command is `1`. |
| `op` | one of the fourteen ops below |
| `arg` | required for `approve`/`revoke` (the 16-hex action digest), forbidden for every other op |
| `at` | RFC 3339, diagnostics only |

`seq` is a decimal string because a u64 does not survive a JSON number
round-trip — doubles carry 53 bits. It is carried the same way in the
existing network-state records for the same reason.

### Ops

`pause`, `resume`, `writes-on`, `writes-off`, `dry-run-on`, `dry-run-off`,
`offline-on`, `offline-off`, `approval-on`, `approval-off`, `approve`,
`revoke`, `shutdown`, `cancel-shutdown`.

The names match the `atperson control` subcommands, including the `on`/`off`
suffix convention used by `writes`, `dry-run` and `offline`. `approve` and
`revoke` carry the action digest in `arg`, and are subject to the same
64-entry bound the local CLI applies.

## The operator must be a separate account

**`ATPERSON_OPERATOR_DID` must not be the entity's own account DID.** This is
enforced, not advised. If the two are the same, the pass is refused: the
entity could author every request it then obeys, so the channel would place
the authority it exists to keep outside the runtime *inside* it.

Nothing is read and nothing is written when they collide. The daemon prints
the reason once per cycle, and `control remote status` and `control remote
poll` both exit non-zero, so a deployment check catches the misconfiguration
rather than discovering it later as an unexplained authority.

The check runs in two places, deliberately. The daemon compares before
opening a session, so a collision costs nothing. The poller compares again
against the DID that actually authenticated, so no caller can pass a
mismatched pair and skip the rule.

| configuration | verdict |
| --- | --- |
| `ATPERSON_OPERATOR_DID` unset | disabled — off by choice, no DID and no session |
| set, and different from the account DID | ready |
| set, and equal to the account DID | **refused** |

Because the operator is always a different account, the entity's own
credentials can never author a control record. `control remote emit`
authenticates as the *operator* — `ATPERSON_OPERATOR_IDENTIFIER` and
`ATPERSON_OPERATOR_APP_PASSWORD` — and the entity's `ATPERSON_IDENTIFIER` /
`ATPERSON_APP_PASSWORD` are not accepted for it. The runtime is structurally
incapable of commanding itself.

## Trust model

Four checks, in this order. The first failure refuses outright.

**0. Separation.** The operator DID is not the account DID. See above.

**1. Provenance.** A record counts only if the at-URI the service returned
carries the configured operator DID as its authority. The authority is read
from the URI, never from the document body: a record cannot claim to be
someone else's. The collection must also be a whole path segment, so a
control record cannot be smuggled in through another collection on the same
repo. A handle authority is not a DID and never matches.

**2. Freshness.** `seq` must be exactly the watermark plus one. A gap means
an earlier command was lost, and applying a later one would silently skip a
pause the operator sequenced first. A repeat is a replay. Either way:
refuse, and change nothing.

**3. Shape.** Unknown op, missing digest on `approve`, or an argument on an
op that takes none: refuse.

A refusal is total. Control state is left byte-identical and the watermark
does not move, so a corrected re-send at the same `seq` still works. This is
why the sequence is the *only* thing that advances the channel, and why a
refused record cannot wedge it.

## Replay safety

The watermark lives in its own file, `<data>/remote-control-cursor.json`,
separate from `control-state.json`. That separation is the point: the cursor
is channel bookkeeping, not an operator switch, and it has to survive a
restart so an old `pause` cannot be replayed past a later `resume`.

A cursor that will not parse aborts the pass. It is never read as zero —
zero means "the next command is `seq` 1", and a broken cursor must not
imply that. `atperson control remote status` exits non-zero and says
`cursor unreadable` in that case.

`save` is the same temp-file-plus-rename-plus-fsync-directory contract the
control state uses, and the cursor is committed *after* the state it
justifies. A crash between the two re-applies an idempotent op on the next
pass rather than skipping one.

## What it cannot do

The channel composes with the existing gates; it does not replace or
bypass any of them.

- `ATPERSON_ALLOW_EXTERNAL_PUBLISHING` and the outbound policy are
  downstream of control state. A remote `writes-on` flips the switch; it
  does not grant a permission.
- `ensure_outbound_allowed` still runs on every write path, so a remote
  `approve` satisfies the approval list and nothing else.
- The envelope, scheduler and journal contracts are untouched.
- Learned C23 state is never reachable from this path. Control state is
  runtime metadata and does not mutate the graph.

A remote operator can pause the entity from any client. They cannot make it
learn something new, and they cannot widen the rate or scope of what it is
allowed to publish beyond what the local CLI would allow from the same host.

## Usage

The channel is inert unless `ATPERSON_OPERATOR_DID` is set. There is no
default: an unset variable means no DID, no commands, and no session opened
for polling.

The operator account is separate from the entity account, so publishing
needs the operator's credentials:

```sh
# the entity's own credentials (the daemon's)
export ATPERSON_IDENTIFIER=entity.example.com
export ATPERSON_APP_PASSWORD=...

# the operator's, used only to publish
export ATPERSON_OPERATOR_IDENTIFIER=operator.example.com
export ATPERSON_OPERATOR_APP_PASSWORD=...
export ATPERSON_OPERATOR_DID=did:plc:operator

# is the channel on, and how far has it advanced?
atperson control remote status

# issue a command from this host, as the operator
atperson control remote emit pause
atperson control remote emit resume
atperson control remote emit approve 0123456789abcdef

# apply now instead of waiting for the next daemon cycle
atperson control remote poll
```

The daemon runs one bounded poll per cycle, before the scheduler, so a
pause issued since the last cycle is in force for the current one rather
than the next. A transport failure or an unusable cursor leaves both files
untouched and the next cycle retries from the same watermark.

| variable | default | meaning |
| --- | --- | --- |
| `ATPERSON_OPERATOR_DID` | *(unset — channel disabled)* | the only DID whose records are commands. **Must differ from the account DID**; equal is refused |
| `ATPERSON_OPERATOR_IDENTIFIER` | — | operator handle, used only by `control remote emit` |
| `ATPERSON_OPERATOR_APP_PASSWORD` | — | operator credential, used only by `control remote emit` |
| `ATPERSON_REMOTE_MAX_RECORDS` | `32` | records fetched per pass |
| `ATPERSON_REMOTE_MAX_APPLIES` | `8` | requests applied per pass; the rest continue next pass |
| `ATPERSON_REMOTE_CONTROL_CURSOR` | `<data>/remote-control-cursor.json` | watermark path |

## Record keys

Request records use a rkey derived from `seq`: fixed-width base-32 in the
TID alphabet, listed in ASCII order, so lexicographic record order equals
sequence order. `reverse` listRecords is therefore genuinely newest-first,
and it is deterministic, so re-emitting a sequence is idempotent under
`putRecord` retry.

## Testing

The trust logic is pure and offline: no service, no clock, `operator_did`
injected. `tests/control/remote.cpp` covers the operator/account separation
rule and its denial text, provenance, the exact-next sequence rule in both
directions (gap and replay), every op mapping onto `ControlState`, the local
approval bound, the argument rule re-checked at apply time, at-URI parsing,
cursor round-trip and restart behaviour, and the rkey/sequence ordering
property the poller depends on. It builds and runs with
`ATPERSON_BUILD_NETWORK=OFF`.
