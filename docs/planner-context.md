# Structured planner context

Planner context is selected in the C23 core. The runtime supplies observations and recent interaction inputs; it does not concatenate an opaque prompt or maintain a second heuristic model of what the entity knows.

The public entry point is `atp_graph_select_context` in `atperson/context.h`.

## Sources

A selection can contain five explicit item kinds:

- `ATP_CONTEXT_ITEM_IMMEDIATE` — the current observed/reply text supplied by the caller;
- `ATP_CONTEXT_ITEM_RECENT` — caller-supplied recent interaction text;
- `ATP_CONTEXT_ITEM_MEMORY` — an episodic memory supported by ranked lexical/learned-association recall;
- `ATP_CONTEXT_ITEM_AUTHOR_STATE` — neutral familiarity/exposure for the current author DID;
- `ATP_CONTEXT_ITEM_SOURCE_STATE` — neutral familiarity/exposure for the current stable source URI.

Every item records its kind, selection reason, normalized score and provenance. Memory items embed the complete `atp_recall_result`; interaction-state items embed the complete `atp_interaction_state`. Immediate/recent text is not copied into a hidden prompt. Instead the item identifies the caller-owned input and records how many leading tokenizer tokens fit the context budget.

Preference/valence state is intentionally absent until the separately tracked preference model exists. The context selector must not invent a preference signal from familiarity.

## Selection scores

Scores are normalized to the range 0..1 so sources can compete under one deterministic ordering:

| Source | Selection score |
| --- | --- |
| Immediate input | `1.0` |
| Recent input | `1 / (1 + age / 86400)` when timestamps are known, otherwise `0` |
| Episodic memory | `recall_score / (recall_score + 1)` |
| Author/source state | neutral `familiarity` |

Equal scores use a fixed source priority: immediate input, episodic memory, recent input, author state, source state. Remaining ties are resolved by observation time, ledger id, source URI, author DID and input index. Fixed learned state plus fixed inputs therefore produce the same ordering.

## Read-only memory consideration

`atp_graph_recall_ranked` represents an actual recall and updates episode recall counters. Planner context must not mutate memory merely by considering it, so context selection uses `atp_graph_recall_ranked_preview` instead.

The preview API uses exactly the same scoring and ordering as ranked recall but does not increment `recall_count` or write `last_recall_at`. It also has an explicit episode-scan bound and examines the most recently retained tail of episodic memory when the configured bound is smaller than the retained memory set.

Recall queries use the shared schema-versioned query/tokenization helper, so observation, action scoring, lookup, semantic recall and planner-context selection all share one token identity contract.

## Bounds

There are hard compile-time ceilings and smaller configurable limits:

- at most 16 selected items;
- at most 32 caller-supplied recent inputs;
- at most 8 selected memories;
- at most 256 textual tokens;
- at most 4096 episodic memories scanned for one selection.

The default policy selects at most 12 items, four recent inputs, four memories, 96 textual tokens and scans at most 256 retained episodes.

Text input is bounded by the same 64 KiB limit as retained ledger payloads. Values above hard limits are rejected rather than silently clamped.

When a selected textual item does not fully fit the remaining token budget, it records both `available_tokens` and `selected_tokens` and sets `truncated`. The selection records aggregate `token_limit_reached`, `item_limit_reached`, `memory_episodes_scanned`, `memory_scan_truncated` and `recent_inputs_scored` metadata for inspection.

Structured numeric interaction state consumes no text-token budget but still consumes an item slot.

## Mutation boundary

Context selection is read-only:

- unknown immediate/recent/query tokens are tokenized but never interned;
- episodic recall counters are unchanged;
- graph nodes, edges, familiarity and neural state are unchanged;
- author/source continuity is derived through the existing read-only interaction API;
- no network I/O occurs.

A later action renderer/planner may consume the structured selection, but it must preserve these explicit provenance and budget decisions rather than replacing them with C++ prompt assembly.
