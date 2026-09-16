# Action inspection

atperson exposes the learned action pipeline through read-only CLI inspection commands. The C++23 runtime only renders structures returned by the authoritative C23 APIs; it does not recompute scores, select different memories, or decide network policy.

Every plan/context/decision trace starts with:

```text
layer: learned-core
network-policy: not-evaluated
```

That distinction is intentional. An accepted learned plan is evidence for later runtime policy, not permission to publish or perform an AT Protocol action.

## One-step candidates

```sh
./build/atperson candidates "alpha delta" 10
```

Each tab-separated line contains:

```text
token  final_score  association_score  familiarity_score  support_score  supporting_observations  context_matches
```

This is a presentation layer over `atp_graph_action_candidates` and remains unchanged from the first inspection slice.

## Bounded plans

```sh
./build/atperson plans "alpha delta" [max-tokens] [beam-width]
```

`max-tokens` and `beam-width` default to the C23 planner defaults and must stay within the same hard core limits (currently 16 tokens and beam width 8). Invalid values are rejected rather than silently clamped.

The output contains each returned plan's mean score, explicit stop reason and every generated step. Each step preserves the complete `atp_action_candidate` evidence: association, familiarity and support scores, supporting observations and context-match count.

Raw plans are intentionally inspectable even when they contain a cycle that would later be rejected by the guarded decision layer.

## Guarded decision trace

```sh
./build/atperson decide "alpha delta" [max-tokens] [beam-width]
```

This calls `atp_graph_action_decide` with the C23 guard defaults. It reports:

- `outcome: plan` or `outcome: abstain`;
- the explicit abstain reason;
- raw and viable plan counts;
- stop reason and triggering step;
- observed candidate/support scores and configured thresholds;
- score-drop evidence;
- repetition counts;
- cycle origin when relevant;
- the accepted plan and its step evidence when a plan survives.

The runtime does not reinterpret any of those values. Guard thresholds and reason codes come directly from C23.

## Structured planner context

```sh
./build/atperson context "alpha delta" [source-id] [author-did]
```

This invokes `atp_graph_select_context` using the supplied text as the immediate input. Supplying a stable AT URI and/or author DID lets the selector also expose matching neutral source/author continuity state. The CLI does not currently invent a recent-interaction window; when a future runtime caller has recent inputs it must supply them explicitly to the same C23 context API.

Each selected item reports its kind, selection reason and normalized score plus token-budget and provenance fields. Episodic memories include the ranked-recall component evidence (exact, association, familiarity, recency and previous-use scores). Author/source items include neutral encounter count, last-seen time, retained-memory count and bounded familiarity.

Memory selection uses the read-only recall-preview path, so merely inspecting context does not increment recall counters.

## Read-only contract

`candidates`, `plans`, `decide`, and `context` are inspection surfaces. They do not:

- train or mutate the graph;
- intern unknown vocabulary;
- update episodic recall counters;
- change familiarity or neural state;
- perform network I/O;
- evaluate outbound policy;
- print credentials implicitly.

Tests exercise rendering and assert aggregate learned-state statistics are unchanged across plan, decision and context inspection.

The existing `recall` command is different: it exercises the use-recording recall API and therefore updates recall counters by design. Use `context` when inspecting planner memory selection without that mutation.

## Output stability

The current output is deliberately human-readable and labelled rather than an undocumented JSON format. Field names are intended for operator/debugging use but are not yet a versioned machine API. If a machine-readable mode is added later, it must have an explicit schema/version rather than encouraging consumers to scrape this text format.
