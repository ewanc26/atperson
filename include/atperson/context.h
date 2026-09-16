#ifndef ATPERSON_CONTEXT_H
#define ATPERSON_CONTEXT_H

#include "atperson/interaction.h"
#include "atperson/memory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_CONTEXT_MAX_ITEMS 16u
#define ATPERSON_CONTEXT_MAX_RECENT_INPUTS 32u
#define ATPERSON_CONTEXT_MAX_MEMORY_ITEMS 8u
#define ATPERSON_CONTEXT_MAX_TOKENS 256u
#define ATPERSON_CONTEXT_MAX_MEMORY_SCAN 4096u
#define ATPERSON_CONTEXT_MAX_TEXT_BYTES ATPERSON_LEDGER_PAYLOAD_LIMIT
#define ATPERSON_CONTEXT_NO_INPUT_INDEX SIZE_MAX

typedef enum atp_context_item_kind {
    ATP_CONTEXT_ITEM_IMMEDIATE = 1,
    ATP_CONTEXT_ITEM_RECENT = 2,
    ATP_CONTEXT_ITEM_MEMORY = 3,
    ATP_CONTEXT_ITEM_AUTHOR_STATE = 4,
    ATP_CONTEXT_ITEM_SOURCE_STATE = 5
} atp_context_item_kind;

typedef enum atp_context_reason {
    ATP_CONTEXT_REASON_IMMEDIATE_INPUT = 1,
    ATP_CONTEXT_REASON_RECENT_INTERACTION = 2,
    ATP_CONTEXT_REASON_EPISODIC_RECALL = 3,
    ATP_CONTEXT_REASON_AUTHOR_FAMILIARITY = 4,
    ATP_CONTEXT_REASON_SOURCE_FAMILIARITY = 5
} atp_context_reason;

/** Caller-owned recent interaction supplied to the context selector. */
typedef struct atp_context_recent_input {
    const char *text;
    const char *source_id;
    const char *author_did;
    uint64_t observed_at;
} atp_context_recent_input;

/** Inputs for one deterministic planner-context selection pass. */
typedef struct atp_context_request {
    const char *immediate_text;
    const char *source_id;
    const char *author_did;
    const atp_context_recent_input *recent;
    size_t recent_count;
    uint64_t at_epoch;
} atp_context_request;

/**
 * Bounded selection policy. Values outside the ATPERSON_CONTEXT_* hard limits
 * are rejected rather than silently clamped.
 */
typedef struct atp_context_config {
    size_t max_items;
    size_t max_recent_items;
    size_t max_memory_items;
    size_t max_tokens;
    size_t max_memory_scan;
} atp_context_config;

/**
 * One selected structured context item.
 *
 * `score` is normalized to 0..1 across sources:
 * - immediate input: 1;
 * - recent input: recency in the same one-day curve used by episodic recall;
 * - episodic memory: recall_score / (recall_score + 1);
 * - author/source state: neutral familiarity.
 *
 * For immediate/recent items, `input_index` identifies the caller-owned input
 * (`ATPERSON_CONTEXT_NO_INPUT_INDEX` for the immediate item, otherwise an
 * index into request.recent). No opaque concatenated prompt is created.
 * `selected_tokens` tells a later renderer/planner how many leading tokens of
 * that input may be consumed; `available_tokens` is the full token count and
 * `truncated` reports token-budget truncation.
 *
 * Memory items embed their ranked-recall evidence and provenance. Interaction
 * items embed neutral derived state; they consume zero text tokens.
 */
typedef struct atp_context_item {
    atp_context_item_kind kind;
    atp_context_reason reason;
    float score;
    size_t selected_tokens;
    size_t available_tokens;
    bool truncated;
    size_t input_index;
    uint64_t observed_at;
    uint64_t ledger_id;
    char source_id[ATPERSON_LEDGER_SOURCE_BYTES];
    char author_did[ATPERSON_LEDGER_AUTHOR_BYTES];
    atp_recall_result recall;
    atp_interaction_state interaction;
} atp_context_item;

/** Complete bounded context selection result. */
typedef struct atp_context_selection {
    atp_context_item items[ATPERSON_CONTEXT_MAX_ITEMS];
    size_t item_count;
    size_t token_count;
    size_t recent_inputs_scored;
    size_t memory_episodes_scanned;
    bool item_limit_reached;
    bool token_limit_reached;
    bool memory_scan_truncated;
} atp_context_selection;

/** Conservative defaults for planner-context selection. */
atp_context_config atp_context_default_config(void);

/**
 * Assemble structured planner context from immediate/recent caller input,
 * read-only episodic recall, and derived author/source interaction state.
 *
 * The operation is deterministic and read-only for fixed graph state and
 * inputs. Unknown text never interns vocabulary. Episodic consideration uses
 * atp_graph_recall_ranked_preview, so recall counters are not modified merely
 * because a memory was considered for a plan.
 *
 * Selection is globally score-ranked with deterministic tie-breaking. The
 * immediate item wins equal-score ties, then memory, recent input, author
 * state, and source state. Remaining ties use observation time, ledger id,
 * provenance strings, and input index.
 *
 * Work is explicitly bounded: request.recent_count may not exceed
 * ATPERSON_CONTEXT_MAX_RECENT_INPUTS, memory scoring examines at most
 * config.max_memory_scan retained episodes, and only config.max_recent_items /
 * config.max_memory_items from those sources may be selected. Item and token
 * budgets are also hard. Textual items may be truncated to the remaining
 * token budget; structured interaction-state items consume zero text tokens.
 */
atp_status atp_graph_select_context(const atp_graph *graph, const atp_context_request *request,
                                    const atp_context_config *config,
                                    atp_context_selection *out);

#ifdef __cplusplus
}
#endif

#endif
