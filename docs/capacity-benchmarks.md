# Capacity and continual-learning benchmarks

Issue #67 tracks evidence for whether larger neural-capacity profiles do useful work rather than merely reserving more memory.

The first benchmark slice lives in `tests/resource/capacity_bench.cpp`. It deliberately crosses the runtime/core boundary:

1. a synthetic host profile is passed through the real C++ resource policy;
2. the resulting `NeuralCapacityRecommendation` is translated into the C23 persisted architecture;
3. the same deterministic observation stream is trained at that topology;
4. learned-state and behavioural measurements are emitted as one JSON object per profile.

No network access is involved.

## Modes

The bounded smoke run covers the current **constrained**, **baseline**, and **capable** classes:

```sh
ctest --test-dir build -R atperson-capacity-smoke --output-on-failure
```

The deliberate benchmark run covers all five current classes, including **large** and **expansive**:

```sh
ctest --test-dir build -L bench -R atperson-capacity-bench --output-on-failure
```

The smoke test is intended for ordinary CI. Timing is reported but never asserted.

## Fixture

Fixture version 1 uses two ordered domains. Domain A learns `moon -> silver` associations before domain B introduces `forest -> green`. The cross-domain token `forest` exists before domain A training without being paired with it, allowing neural discrimination to be measured before and after the domain shift.

Each profile reports:

- capacity-policy version and exact persisted topology;
- shared parameter count and estimated authoritative neural learned-state bytes;
- observation/training counts and mean loss;
- the domain-A score before and after domain-B learning;
- domain-B and cross-domain neural scores;
- retention delta and domain-B discrimination margin;
- recall matches/returns;
- action-candidate count, top candidate and score;
- exact snapshot/restart equivalence;
- exact learned-state reconstruction from the durable observation ledger;
- withdrawal followed by a fresh ledger rebuild, including excluded-entry accounting;
- wall-clock duration.

The same fixture is run twice for every profile. All learned metrics must match exactly. Wall-clock duration is excluded from the determinism comparison.

Each fixture now also records its observations through the real append-only ledger. Before recall mutates episodic usage counters, a fresh graph at the same persisted topology replays that ledger and must reproduce graph counts, mean loss and the measured neural pair scores exactly. The fixture then withdraws one repeated domain-B observation and performs another fresh rebuild; the replay report must name exactly one withdrawn entry and the rebuilt observation count must fall by exactly one.

The benchmark does **not** assert that a larger profile must score better. That is intentional: #67 is supposed to expose when extra capacity is not worth its cost rather than baking monotonic improvement into the test.

## Next benchmark slices

This foundation does not close #67. Follow-up work still needs to add:

- longer multi-domain continual-learning streams and held-out evaluation;
- explicit plasticity-control comparisons;
- richer semantic-recall quality metrics rather than return counts alone;
- guarded planning/abstention quality fixtures;
- resident/peak process memory where portable measurement is available;
- larger deliberate benchmark presets that can expose capacity differences more clearly.

No aggregate "personhood" or consciousness score should be introduced. The benchmark should report concrete learned and behavioural capabilities only.
