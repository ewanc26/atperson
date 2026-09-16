# Action abstention and termination

The raw beam planner (`atp_graph_action_plans`) remains an inspection primitive: it is bounded, deterministic and exposes candidate evidence, but it will follow a strong learned cycle until `max_tokens` because raw planning deliberately does not contain policy.

`atp_graph_action_decide` is the guarded C23 decision layer used when the system needs to decide whether learned evidence supports a plan at all. It does not publish anything and contains no AT Protocol or outbound-policy rules.

## Outcomes

A decision has two top-level outcomes:

- an accepted plan, possibly deliberately shortened by a guard; or
- explicit abstention before any token is accepted.

Abstention reasons are inspectable:

- `ATP_ACTION_ABSTAIN_EMPTY_CONTEXT` — the supplied context is the empty string;
- `ATP_ACTION_ABSTAIN_NO_CANDIDATES` — the learned graph has no continuation for the context;
- `ATP_ACTION_ABSTAIN_LOW_SCORE` — every raw plan's first candidate fails the configured candidate-score floor;
- `ATP_ACTION_ABSTAIN_LOW_SUPPORT` — every raw plan's first viable candidate fails the configured support floor.

An abstention is not an error. It is a successful decision that there is insufficient learned evidence to produce supported output.

## Guarded stop reasons

After at least one token has been accepted, a plan can stop for the raw planner's existing reasons (`DEAD_END`, `MAX_TOKENS`) or because a later candidate triggers one of the guards:

- `LOW_SCORE` — candidate score is below `min_candidate_score`;
- `LOW_SUPPORT` — support score is below `min_support_score`;
- `SCORE_DROP` — candidate score falls by more than `max_score_drop` from the previous accepted step;
- `REPETITION` — accepting the token would exceed the configured consecutive-occurrence bound;
- `CYCLE` — the token revisits an earlier non-consecutive generated token.

The triggering candidate is not added to the accepted plan. This means a useful supported prefix can survive even when a later continuation becomes repetitive or weak.

Cycle detection is always active. Consecutive repetition is separately configurable between one and two occurrences so a caller can permit one doubled token without allowing an arbitrary loop.

## Default thresholds

`atp_action_guard_default_config()` returns:

| Guard | Default |
| --- | ---: |
| minimum candidate score | `0.15` |
| minimum support score | `0.25` |
| maximum step-to-step score drop | `0.40` |
| maximum consecutive occurrences | `1` |

Score thresholds and the drop bound must be finite and in `[0, 1]`. The consecutive-occurrence setting must be between `1` and `ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES` (currently `2`). Invalid values are rejected; they are never silently clamped.

The defaults are intentionally compatible with sparse early experience: a single supporting edge has support score `0.5`, so one observation is not rejected solely for being the first observation. The system can still use stricter settings when appropriate.

## Evidence

Every guarded decision includes `atp_action_stop_evidence` with the configured thresholds plus the observed values that caused the result:

- raw step index;
- accepted prefix length;
- triggering token when there is one;
- candidate score and configured minimum;
- support score and configured minimum;
- previous candidate score and maximum allowed drop;
- observed/configured consecutive occurrence counts;
- cycle start index when a non-consecutive cycle is detected;
- planner maximum-token bound.

For ordinary `DEAD_END` and `MAX_TOKENS` termination there is no rejected candidate; the evidence records the accepted length and the last accepted token/score when available.

## Determinism and mutation boundary

The decision layer obtains the raw planner's bounded plans, guards every returned plan independently, then deterministically ranks the surviving guarded plans using the same score/length/token ordering principles as the planner. If every raw plan is rejected at step zero, the strongest raw plan's rejection evidence determines the abstain reason.

The operation is read-only. It does not update graph nodes, edges, neural state, memory, familiarity or runtime state. It performs no network I/O.

An accepted plan is evidence for later policy only. It is never permission to post, reply, like, follow, repost, send a message or perform moderation. Those gates remain C++23 runtime policy and eventual Wolfram-backed execution work.
