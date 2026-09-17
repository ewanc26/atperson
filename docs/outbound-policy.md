# Outbound action policy and rate budgets

This document describes the runtime decision layer that sits between an
accepted C23 core plan and any AT Protocol write. It is the `#23` layer: the
runtime's own action vocabulary plus an operator-authored policy and durable
rate budgets.

## Boundary

A core plan is evidence, never permission.

- The C23 core scores and plans. It owns no AT Protocol permissions, no
  moderation choices and no outbound rate policy.
- The runtime's outbound layer owns the *action* vocabulary
  (`post`, `reply`, `like`, `repost`, `follow`, `unfollow`, `moderation`),
  the operator policy, the rate budgets and the decision codes.
- Wolfram owns protocol mechanics. This layer never calls Wolfram and never
  performs network I/O.
- The outbound policy and budget are **runtime metadata**, not learned state.
  They live outside the model snapshot and outside the state-directory writer
  lock, next to operator `control` state.

The layer is currently **inspection and admission only**. It evaluates and
records decisions so that a future Wolfram-backed write path has a single,
inspectable gate to call. It does not itself post, reply, like, follow,
repost or moderate.

## Default-deny

Nothing is permitted until an operator explicitly enables a kind.

- A missing policy file yields the fail-closed default: every kind disabled.
- A present policy file must fully parse and validate or the command fails;
  it is never silently reinterpreted.
- An unknown action kind at the CLI boundary is denied in the same inspectable
  shape (`reason: unsupported_kind`) with no state mutation.
- A missing budget file means "nothing recorded yet". A present but malformed
  budget file is an error, so a caller refuses rather than guessing.

## Policy file

Path: `ATPERSON_OUTBOUND_POLICY` (default `<data>/outbound-policy.json`).

```json
{
  "format": "atperson-outbound-policy",
  "version": 1,
  "kinds": {
    "reply": {
      "enabled": true,
      "max_in_window": 5,
      "window_seconds": 86400,
      "min_interval_seconds": 300,
      "duplicate_cooldown_seconds": 604800
    }
  }
}
```

Every field of a listed kind is required:

| Field | Meaning |
| --- | --- |
| `enabled` | default-deny switch for this kind |
| `max_in_window` | maximum admitted actions in the trailing window |
| `window_seconds` | trailing window length, `1` .. 366 days |
| `min_interval_seconds` | minimum spacing between admitted actions (`0` = none) |
| `duplicate_cooldown_seconds` | suppression window for an identical action identity (`0` = off) |

Kinds not listed in `kinds` stay disabled. Unknown kind names, duplicate
keys and out-of-range limits are hard errors (`OutboundPolicyError`). The
document is versioned; `version` other than `1` is rejected.

## Budget state

Path: `ATPERSON_OUTBOUND_BUDGET` (default `<data>/outbound-budget.json`).

The budget is a rolling record of recently admitted actions. It is written
atomically (write-and-rename) so a crash or restart cannot reset a window and
permit a burst: recorded timestamps are reloaded and the same limits apply.

Per kind it stores the last admitted time, the recent admitted timestamps
inside the configured horizon, and recently admitted action identities for
duplicate suppression. Records are pruned to the current window/spacing/
cooldown horizon and capped per kind (`256`), so the file and each evaluation
stay bounded. Timestamps are monotonic per kind: a backwards clock is clamped
to the last recorded action so a window can never shrink.

## Decisions

`evaluate` is read-only. `admit` is the single admission point a real write
path must call: it evaluates and, only on `allow`, records the action. It is
single-writer by contract; the returned budget status is the state measured
*before* admission.

| Outcome | Reason code | When |
| --- | --- | --- |
| `allow` | `allow` | within every limit and not a duplicate |
| `deny` | `kind_disabled` | the kind is not enabled in the policy |
| `deny` | `unsupported_kind` | the kind name is not known to the runtime |
| `defer` | `duplicate_suppressed` | identical identity inside the duplicate cooldown |
| `defer` | `cooldown_active` | inside the minimum spacing since the last action |
| `defer` | `window_exhausted` | the trailing window is full |

`defer` carries `retry_after_seconds`; `deny` is a permanent refusal for this
proposal. Every decision carries the budget status used, so the verdict is
explainable without re-deriving state.

## CLI

```sh
./build/atperson outbound status [kind]     # enabled/disabled + current budget
./build/atperson outbound rules             # the effective policy document
./build/atperson outbound evaluate <kind> [target] [digest]
./build/atperson outbound admit <kind> [target] [digest]
```

`evaluate` never mutates state; `admit` consumes budget only on `allow` and
then persists the budget. Both print the decision, the budget behind it and
the current operator `control` gate (paused, writes disabled, dry-run,
unapproved digest). They call no Wolfram code and perform no network writes.

`target` is the stable AT URI or DID the action applies to (empty for an
original post); for a like, repost or reply it is the subject record URI, for
a follow/unfollow the subject DID. `digest` is the `#22` approval digest that
binds the exact inspected decision; kind + target + digest is the action
identity used for duplicate suppression.

## Tests

`tests/outbound/outbound.cpp` (target `atperson-outbound`, offline) covers
kind round-trips, default-deny, policy round-trip and rejection, window
exhaustion and recovery, minimum spacing, duplicate suppression, backwards
clocks, restart persistence, missing/corrupt budget files, concurrent
proposals serialising at admission, bounded/pruned records, unsupported
kinds, and the CLI surface including control-gate reporting.
