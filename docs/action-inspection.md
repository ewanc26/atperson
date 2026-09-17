# Action inspection

The planner is inspectable without asking the runtime to reinterpret its results. `atperson` exposes the C23 candidate, planning and decision surfaces directly through read-only CLI commands.

Every trace starts by making the boundary explicit:

```text
layer: learned-core
network-policy: not-evaluated
```

That is not decoration. A plan accepted by the learned core is still only evidence; outbound policy and operator control decide whether anything may happen on the network.

## One-step candidates

```sh
./build/atperson candidates "alpha delta" 10
```

Each result reports:

```text
token  final_score  association_score  familiarity_score  support_score  supporting_observations  context_matches
```

The C++23 layer renders the `atp_graph_action_candidates` result. It does not recompute the score.

## Bounded plans

```sh
./build/atperson plans "alpha delta" [max-tokens] [beam-width]
```

The optional bounds use the C23 defaults and must stay inside the same hard limits, currently 16 generated tokens and a beam width of 8. Invalid values are rejected rather than clamped.

Each plan includes its mean score, stop reason and complete step evidence. Raw planning is deliberately policy-free, so a strong cycle may remain visible here even when the guarded decision layer would reject or shorten it.

## Guarded decisions

```sh
./build/atperson decide "alpha delta" [max-tokens] [beam-width] [min-candidate] [min-support] [max-drop] [max-consecutive]
```

This calls `atp_graph_action_decide` with the C23 guard configuration. The default thresholds are `0.15`, `0.25`, `0.40` and `1` respectively.

The trace reports:

- `plan` or `abstain`;
- the explicit abstention/stop reason;
- raw and viable plan counts;
- effective thresholds;
- triggering step and observed evidence;
- score-drop, repetition and cycle evidence;
- the complete accepted prefix when one survives.

The runtime does not reinterpret these fields.

## Structured context

```sh
./build/atperson context "alpha delta" [source-id] [author-did]
```

This calls `atp_graph_select_context`. Supplying a stable source URI and/or author DID lets the selector include neutral source/author continuity alongside immediate text and episodic memory.

Every selected item reports its kind, reason, score, budget use and provenance. Memory items expose their recall evidence; author/source items expose neutral encounter/familiarity state.

Context selection uses recall preview, so inspecting it does not increment episodic recall counters.

## Read-only contract

`candidates`, `plans`, `decide` and `context` do not:

- train or mutate the graph;
- intern unknown vocabulary;
- change familiarity, valence or neural state;
- update episodic recall counters through context preview;
- perform AT Protocol I/O;
- evaluate outbound policy.

`recall` is different: it uses the use-recording recall API and therefore updates recall counters by design.

## Output stability

The current output is operator-facing text, not a versioned machine protocol. Consumers should not scrape it as though it were one. Any future machine-readable mode needs an explicit schema and version.