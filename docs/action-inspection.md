# Action inspection

The runtime exposes the current one-step action evidence through a read-only `candidates` command:

```sh
./build/atperson candidates "alpha delta" 10
```

Each line is tab-separated and contains:

```text
token  final_score  association_score  familiarity_score  support_score  supporting_observations  context_matches
```

The command is a presentation layer over the C23 `atp_graph_action_candidates` API. It does not recompute scores in C++, train the graph, add unknown context tokens, or perform network I/O.

## Bounded sequence planning

The C23 action layer also exposes `atp_graph_action_plans`, a deterministic bounded beam search over the same candidate evidence. The default planner generates at most eight tokens with beam width four; hard API limits are 16 tokens and width eight. The initial context is capped at 4096 bytes and invalid limits are rejected rather than silently clamped.

The first step uses the caller's complete context. Each later step uses the immediately preceding generated token as its context. That deliberately keeps the first planner narrow and graph-native: richer episodic, interaction, author, and conversation context belongs in the structured context-selection work rather than being hidden in an ad-hoc C++ prompt.

Every generated token retains its full `atp_action_candidate` evidence. A plan's score is the mean of its step scores, and its mechanical stop reason is explicit:

- `ATP_ACTION_PLAN_STOP_DEAD_END` when the final token has no learned continuation;
- `ATP_ACTION_PLAN_STOP_MAX_TOKENS` when the configured sequence bound is reached.

Cycles therefore remain inspectable but cannot run indefinitely. Unknown initial context yields no plan, and planning is read-only: it does not alter vocabulary, familiarity, graph weights, memories, or training counters.

The normal executable does not yet render full plan traces. Issue #18 extends the CLI inspection surface now that the core planning primitive exists; abstention/repetition policy and richer termination reasons remain separate follow-on work rather than being folded invisibly into this beam search.
