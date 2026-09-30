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
- `ATP_ACTION_ABSTAIN_LOW_SUPPORT` — every viable first candidate is below the support floor;
- `ATP_ACTION_ABSTAIN_NEGATIVE_VALENCE` — every viable first candidate was vetoed by the opt-in valence guard (see below).

Abstention is a valid decision, not an error condition.

## Stop guards

After at least one accepted token, generation can stop because the raw planner reached `DEAD_END`/`MAX_TOKENS` or because a later candidate trips a guard:

- `LOW_SCORE` — below `min_candidate_score`;
- `LOW_SUPPORT` — below `min_support_score`;
- `SCORE_DROP` — falls too far from the previous accepted step;
- `REPETITION` — exceeds the configured consecutive occurrence bound;
- `NEGATIVE_VALENCE` — the opt-in valence guard vetoed a token with recorded negative experience;
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
| valence guard | off (`min_valence` `-0.25` if enabled) |

Score/drop thresholds must be finite and inside `[0, 1]`. Invalid values are rejected rather than silently adjusted.

The defaults are intentionally usable with sparse early experience: a single supporting edge has support score `0.5`, so the first observation is not rejected merely for being the first one.

## Valence guard (opt-in)

Valence is the entity's explicit, experience-derived value for a token (see [`valence.md`](valence.md)). Every candidate now reports it, as `valence` and `valence_events`, next to its other components. It is **not** part of `score`: ranking and the score formula are unchanged, and a token that never received an explicit valence event reports `0` / `0` (exposure alone gives no valence).

The guard is off by default. When `atp_action_guard_config.valence_guard` is on, a candidate is rejected (`NEGATIVE_VALENCE`) if it has at least one valence event and its valence is below `min_valence`. Two properties keep it honest:

- `min_valence` must be finite and in `[-1, 0]`, so the guard can only veto recorded negative experience. It can never demand positive valence, and a token with no valence events is never blocked.
- `min_valence` is validated even while the guard is off, so a malformed config cannot become valid by flipping one flag.

A veto is an ordinary guard outcome: on the first step it abstains, later it truncates the accepted prefix, and the evidence carries `candidate_valence`, `valence_guard` and `min_valence`. It only narrows what may be proposed. It grants no permission and changes no outbound gate.

Operators opt in with `ATPERSON_DECISION_MIN_VALENCE` (a number in `[-1, 0]`; unset or empty leaves it off; anything else is an error). The setting applies to `decide`, `audit` and the autonomous scheduler alike. The library default threshold, used when a caller enables the guard without choosing one, is `-0.25`.

## Evidence

Every guarded decision exposes `atp_action_stop_evidence`, including the configured thresholds and the values that caused the result. Depending on the stop reason this includes the raw step, accepted length, triggering token, candidate/support scores, previous score, repetition count, cycle origin and planner token limit.

For `DEAD_END` and `MAX_TOKENS`, there is no rejected candidate; the evidence describes the accepted prefix instead.

## Determinism and mutation

Every raw plan is guarded independently and surviving plans are ranked deterministically using the planner's score/length/token ordering. If all raw plans fail at step zero, the strongest rejected plan supplies the abstention evidence.

The operation is read-only. It does not change learned state or perform network I/O.

An accepted result still does not mean "publish this". Runtime outbound policy, operator control and the separate execution path remain responsible for network permission.