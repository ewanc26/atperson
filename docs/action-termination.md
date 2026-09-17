# Action abstention and termination

The raw beam planner is useful for inspection, but it is deliberately not a policy engine. If the graph contains a strong learned cycle, raw planning may follow it until `max_tokens`.

`atp_graph_action_decide` adds the C23 guard layer that answers a narrower question: does the learned evidence support a usable plan at all? It still performs no network action and knows nothing about AT Protocol permissions.

## Outcomes

A guarded decision returns either:

- an accepted plan, possibly shortened by a guard; or
- explicit abstention before the first token is accepted.

Abstention reasons are stable and inspectable:

- `ATP_ACTION_ABSTAIN_EMPTY_CONTEXT` — no input context;
- `ATP_ACTION_ABSTAIN_NO_CANDIDATES` — no learned continuation exists;
- `ATP_ACTION_ABSTAIN_LOW_SCORE` — every first candidate is below the configured score floor;
- `ATP_ACTION_ABSTAIN_LOW_SUPPORT` — every viable first candidate is below the support floor.

Abstention is a valid decision, not an error condition.

## Stop guards

After at least one accepted token, generation can stop because the raw planner reached `DEAD_END`/`MAX_TOKENS` or because a later candidate trips a guard:

- `LOW_SCORE` — below `min_candidate_score`;
- `LOW_SUPPORT` — below `min_support_score`;
- `SCORE_DROP` — falls too far from the previous accepted step;
- `REPETITION` — exceeds the configured consecutive occurrence bound;
- `CYCLE` — revisits an earlier non-consecutive generated token.

The triggering token is not appended. A supported prefix can therefore survive even when the next continuation becomes weak or repetitive.

Cycle detection is always enabled. Consecutive repetition is separately configurable up to `ATPERSON_ACTION_MAX_CONSECUTIVE_OCCURRENCES`, currently 2.

## Defaults

`atp_action_guard_default_config()` returns:

| Guard | Default |
| --- | ---: |
| minimum candidate score | `0.15` |
| minimum support score | `0.25` |
| maximum step-to-step score drop | `0.40` |
| maximum consecutive occurrences | `1` |

Score/drop thresholds must be finite and inside `[0, 1]`. Invalid values are rejected rather than silently adjusted.

The defaults are intentionally usable with sparse early experience: a single supporting edge has support score `0.5`, so the first observation is not rejected merely for being the first one.

## Evidence

Every guarded decision exposes `atp_action_stop_evidence`, including the configured thresholds and the values that caused the result. Depending on the stop reason this includes the raw step, accepted length, triggering token, candidate/support scores, previous score, repetition count, cycle origin and planner token limit.

For `DEAD_END` and `MAX_TOKENS`, there is no rejected candidate; the evidence describes the accepted prefix instead.

## Determinism and mutation

Every raw plan is guarded independently and surviving plans are ranked deterministically using the planner's score/length/token ordering. If all raw plans fail at step zero, the strongest rejected plan supplies the abstention evidence.

The operation is read-only. It does not change learned state or perform network I/O.

An accepted result still does not mean "publish this". Runtime outbound policy, operator control and the separate execution path remain responsible for network permission.