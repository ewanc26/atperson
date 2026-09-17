# Outbound policy and rate budgets

An accepted plan from the C23 core is evidence, not permission to touch the network. The outbound policy is the runtime gate between learned decisions and an AT Protocol write.

The policy layer does not perform network I/O itself. It owns the action vocabulary, operator-authored rules and durable rate budgets; [`outbound-execution.md`](outbound-execution.md) describes how `atperson publish` consumes those decisions before Wolfram is allowed to write anything.

## Boundary

The split is intentionally boring and explicit:

- **C23** scores and plans from learned state. It owns no AT Protocol permissions or rate policy.
- **The C++23 runtime** owns outbound action kinds, policy, rate budgets and stable decision reasons.
- **Wolfram** owns the actual AT Protocol mechanics.
- **Operator policy and budgets are runtime metadata**, not learned state. They are not stored in model snapshots and cannot become personality by accident.

The current runtime vocabulary is `post`, `reply`, `like`, `repost`, `follow`, `unfollow` and `moderation`. The operator-led execution path currently implements posts and replies; unsupported write kinds stay closed rather than being silently translated into something else.

## Default deny

Nothing is allowed merely because a plan exists.

- If the policy file is missing, every action kind is disabled.
- A policy file must parse and validate completely or evaluation fails.
- Unknown action kinds are denied as `unsupported_kind` without mutating state.
- A missing budget file means there is no previous budget history. A malformed budget file is an error; the runtime does not guess.

## Policy file

`ATPERSON_OUTBOUND_POLICY` defaults to `<data>/outbound-policy.json`.

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

Every field on a listed kind is required:

| Field | Meaning |
| --- | --- |
| `enabled` | Explicit on/off switch for the kind |
| `max_in_window` | Maximum admitted actions in the trailing window |
| `window_seconds` | Trailing window length, from 1 second to 366 days |
| `min_interval_seconds` | Minimum spacing between admitted actions (`0` disables it) |
| `duplicate_cooldown_seconds` | Suppression window for an identical action identity (`0` disables it) |

Kinds omitted from `kinds` remain disabled. Unknown kind names, duplicate keys, unsupported versions and out-of-range values are hard errors.

## Durable budget state

`ATPERSON_OUTBOUND_BUDGET` defaults to `<data>/outbound-budget.json`.

The budget records recently admitted actions and is replaced atomically. Restarting the process therefore does not reset a rate window and accidentally permit a burst.

For each kind it keeps the last admitted time, recent admitted timestamps and recent action identities used for duplicate suppression. Old entries are pruned to the active policy horizon and each kind is capped at 256 records. If the system clock moves backwards, evaluation clamps to the last recorded time instead of letting the window shrink.

## Decisions

`evaluate` is read-only. `admit` is the state-changing admission point used by a real write path.

| Outcome | Reason | Meaning |
| --- | --- | --- |
| `allow` | `allow` | All configured limits pass |
| `deny` | `kind_disabled` | The operator has not enabled the kind |
| `deny` | `unsupported_kind` | The runtime does not know the action kind |
| `defer` | `duplicate_suppressed` | The same action identity is still in its duplicate cooldown |
| `defer` | `cooldown_active` | The minimum interval has not elapsed |
| `defer` | `window_exhausted` | The rolling window is full |

A deferred result includes `retry_after_seconds`. Every result carries the budget state used to make it, so an operator can inspect the decision without reconstructing it from the budget file.

## CLI

```sh
./build/atperson outbound status [kind]
./build/atperson outbound rules
./build/atperson outbound evaluate <kind> [target] [digest]
./build/atperson outbound admit <kind> [target] [digest]
```

`target` is the stable AT URI or DID the action applies to, and may be empty for an original post. `digest` is the control approval digest binding the exact inspected action. `kind + target + digest` forms the identity used for duplicate suppression.

The inspection commands also show the current operator control gate. They do not call Wolfram and do not perform network writes.

## Relationship to `publish`

`atperson publish` evaluates this policy before it creates a Wolfram session. A denial or deferral stops there. The budget is only recorded after a confirmed successful write, so a failed or ambiguous network attempt can be retried with the same frozen action without consuming quota.

See [`outbound-execution.md`](outbound-execution.md) for the complete gate order and idempotency contract.

## Tests

`tests/outbound/outbound.cpp` covers the default-deny path, policy parsing, window exhaustion and recovery, minimum spacing, duplicate suppression, backwards clocks, restart persistence, malformed state, bounded pruning, unsupported kinds and the CLI surface. All of that coverage is offline.