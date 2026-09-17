# Structured planner context

Planner context is assembled in the C23 core rather than by concatenating an opaque runtime prompt. The runtime supplies explicit inputs; the core chooses from those inputs and learned evidence under deterministic bounds.

The public API is `atp_graph_select_context` in `atperson/context.h`.

## Context sources

A selection may contain five item kinds:

- `ATP_CONTEXT_ITEM_IMMEDIATE` — current input text;
- `ATP_CONTEXT_ITEM_RECENT` — caller-supplied recent interaction text;
- `ATP_CONTEXT_ITEM_MEMORY` — episodic memory supported by ranked recall;
- `ATP_CONTEXT_ITEM_AUTHOR_STATE` — neutral continuity for an author DID;
- `ATP_CONTEXT_ITEM_SOURCE_STATE` — neutral continuity for a stable source URI.

Every item carries its kind, selection reason, normalised score and provenance. Memory entries embed their full recall evidence; author/source entries embed their derived interaction state.

Immediate and recent text remain caller-owned. The selector records how many leading tokenizer tokens fit the budget rather than building a hidden C++ string prompt.

Valence/preference is not smuggled in through familiarity. Familiarity remains a neutral exposure signal.

## Selection scores

| Source | Score |
| --- | --- |
| Immediate input | `1.0` |
| Recent input | `1 / (1 + age / 86400)` when timestamps are known, otherwise `0` |
| Episodic memory | `recall_score / (recall_score + 1)` |
| Author/source state | neutral `familiarity` |

Equal scores use a fixed source priority: immediate, episodic memory, recent input, author state, source state. Remaining ties use observation time, ledger id, source URI, author DID and input index.

Fixed learned state plus fixed inputs therefore gives fixed ordering.

## Recall preview

Normal ranked recall records the fact that a memory was used. Context assembly must be able to *consider* memories without mutating them, so it uses `atp_graph_recall_ranked_preview`.

Preview uses the same scoring/ordering but does not increment recall counters or update the last-recall timestamp. It also has an explicit scan bound and, when bounded, examines the most recent retained tail.

All query paths share the same schema-versioned tokenisation contract.

## Bounds

Hard ceilings currently include:

- 16 selected items;
- 32 caller-supplied recent inputs;
- 8 selected memories;
- 256 textual tokens;
- 4096 episodic memories scanned.

The default policy is smaller: 12 items, four recent inputs, four memories, 96 textual tokens and 256 scanned episodes.

Inputs above hard limits are rejected rather than silently clamped. A selected text item that only partly fits records both available and selected token counts plus a truncation flag.

The final selection also exposes aggregate budget/truncation metadata. Numeric interaction-state items consume an item slot but no text-token budget.

## Mutation boundary

Context selection is read-only. It does not intern unknown tokens, mutate recall counters, change graph/neural/familiarity state or perform network I/O.

Later planning can consume the structured result, but should preserve the provenance and budget decisions instead of replacing them with hidden runtime prompt assembly.