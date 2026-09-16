# Action inspection

The runtime exposes the current Stage 5 one-step action evidence through a read-only `candidates` command:

```sh
./build/atperson candidates "alpha delta" 10
```

Each line is tab-separated and contains:

```text
token  final_score  association_score  familiarity_score  support_score  supporting_observations  context_matches
```

The command is a presentation layer over the C23 `atp_graph_action_candidates` API. It does not recompute scores in C++, train the graph, add unknown context tokens, or perform network I/O.

This is intentionally narrower than a full decision trace. Sequence planning, context assembly, and abstention/termination traces will extend this inspection surface after the corresponding core APIs exist.
