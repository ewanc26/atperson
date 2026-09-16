#include "atperson/context.h"

#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ATP_CONTEXT_CANDIDATE_CAPACITY \
    (1u + ATPERSON_CONTEXT_MAX_RECENT_INPUTS + ATPERSON_CONTEXT_MAX_MEMORY_ITEMS + 2u)

typedef struct atp_context_token_count {
    size_t count;
} atp_context_token_count;

static bool atp_context_count_emit(void *userdata, const char *token) {
    (void)token;
    atp_context_token_count *counter = userdata;
    if (counter->count != SIZE_MAX) {
        counter->count++;
    }
    return true;
}

static size_t atp_context_count_tokens(const char *text) {
    if (!text || text[0] == '\0') {
        return 0u;
    }
    atp_context_token_count counter = {0};
    atp_tokenize(text, ATPERSON_SCHEMA_VERSION, atp_context_count_emit, &counter);
    return counter.count;
}

static bool atp_context_string_within(const char *text, size_t max_bytes) {
    if (!text) {
        return true;
    }
    for (size_t i = 0u; i <= max_bytes; ++i) {
        if (text[i] == '\0') {
            return true;
        }
    }
    return false;
}

static bool atp_context_identifier_fits(const char *identifier, size_t capacity) {
    if (!identifier) {
        return true;
    }
    for (size_t i = 0u; i < capacity; ++i) {
        if (identifier[i] == '\0') {
            return true;
        }
    }
    return false;
}

static void atp_context_copy_identifier(char *out, size_t capacity, const char *identifier) {
    if (!out || capacity == 0u) {
        return;
    }
    out[0] = '\0';
    if (!identifier || identifier[0] == '\0') {
        return;
    }
    const size_t length = strlen(identifier);
    memcpy(out, identifier, length + 1u);
}

static float atp_context_recency(uint64_t observed_at, uint64_t at_epoch) {
    if (observed_at == 0u || at_epoch == 0u) {
        return 0.0f;
    }
    const uint64_t age = at_epoch > observed_at ? at_epoch - observed_at : 0u;
    const double scaled =
        (double)age / (double)ATPERSON_RECALL_RECENCY_WINDOW_SECONDS;
    return (float)(1.0 / (1.0 + scaled));
}

static float atp_context_normalize_positive(float value) {
    if (!(value > 0.0f) || !isfinite(value)) {
        return 0.0f;
    }
    return value / (value + 1.0f);
}

static int atp_context_kind_priority(atp_context_item_kind kind) {
    switch (kind) {
    case ATP_CONTEXT_ITEM_IMMEDIATE:
        return 0;
    case ATP_CONTEXT_ITEM_MEMORY:
        return 1;
    case ATP_CONTEXT_ITEM_RECENT:
        return 2;
    case ATP_CONTEXT_ITEM_AUTHOR_STATE:
        return 3;
    case ATP_CONTEXT_ITEM_SOURCE_STATE:
        return 4;
    default:
        return 5;
    }
}

static int atp_context_item_compare(const void *left, const void *right) {
    const atp_context_item *a = left;
    const atp_context_item *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }

    const int a_priority = atp_context_kind_priority(a->kind);
    const int b_priority = atp_context_kind_priority(b->kind);
    if (a_priority < b_priority) {
        return -1;
    }
    if (a_priority > b_priority) {
        return 1;
    }

    if (a->observed_at < b->observed_at) {
        return 1;
    }
    if (a->observed_at > b->observed_at) {
        return -1;
    }
    if (a->ledger_id < b->ledger_id) {
        return 1;
    }
    if (a->ledger_id > b->ledger_id) {
        return -1;
    }

    const int source_order = strcmp(a->source_id, b->source_id);
    if (source_order != 0) {
        return source_order;
    }
    const int author_order = strcmp(a->author_did, b->author_did);
    if (author_order != 0) {
        return author_order;
    }
    if (a->input_index < b->input_index) {
        return -1;
    }
    if (a->input_index > b->input_index) {
        return 1;
    }
    return 0;
}

static bool atp_context_config_valid(const atp_context_config *config) {
    return config->max_items > 0u && config->max_items <= ATPERSON_CONTEXT_MAX_ITEMS &&
           config->max_recent_items <= ATPERSON_CONTEXT_MAX_RECENT_INPUTS &&
           config->max_memory_items <= ATPERSON_CONTEXT_MAX_MEMORY_ITEMS &&
           config->max_tokens > 0u && config->max_tokens <= ATPERSON_CONTEXT_MAX_TOKENS &&
           config->max_memory_scan <= ATPERSON_CONTEXT_MAX_MEMORY_SCAN;
}

static bool atp_context_request_valid(const atp_context_request *request) {
    if (request->recent_count > ATPERSON_CONTEXT_MAX_RECENT_INPUTS ||
        (request->recent_count > 0u && !request->recent) ||
        !atp_context_string_within(request->immediate_text, ATPERSON_CONTEXT_MAX_TEXT_BYTES) ||
        !atp_context_identifier_fits(request->source_id, ATPERSON_LEDGER_SOURCE_BYTES) ||
        !atp_context_identifier_fits(request->author_did, ATPERSON_LEDGER_AUTHOR_BYTES)) {
        return false;
    }

    for (size_t i = 0u; i < request->recent_count; ++i) {
        const atp_context_recent_input *recent = &request->recent[i];
        if (!atp_context_string_within(recent->text, ATPERSON_CONTEXT_MAX_TEXT_BYTES) ||
            !atp_context_identifier_fits(recent->source_id, ATPERSON_LEDGER_SOURCE_BYTES) ||
            !atp_context_identifier_fits(recent->author_did, ATPERSON_LEDGER_AUTHOR_BYTES)) {
            return false;
        }
    }
    return true;
}

atp_context_config atp_context_default_config(void) {
    return (atp_context_config){
        .max_items = 12u,
        .max_recent_items = 4u,
        .max_memory_items = 4u,
        .max_tokens = 96u,
        .max_memory_scan = 256u,
    };
}

atp_status atp_graph_select_context(const atp_graph *graph, const atp_context_request *request,
                                    const atp_context_config *config,
                                    atp_context_selection *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!graph || !request || !out) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const atp_context_config selection_config =
        config ? *config : atp_context_default_config();
    if (!atp_context_config_valid(&selection_config) || !atp_context_request_valid(request)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_context_item candidates[ATP_CONTEXT_CANDIDATE_CAPACITY] = {0};
    size_t candidate_count = 0u;

    if (request->immediate_text && request->immediate_text[0] != '\0') {
        const size_t tokens = atp_context_count_tokens(request->immediate_text);
        if (tokens > 0u) {
            atp_context_item *item = &candidates[candidate_count++];
            item->kind = ATP_CONTEXT_ITEM_IMMEDIATE;
            item->reason = ATP_CONTEXT_REASON_IMMEDIATE_INPUT;
            item->score = 1.0f;
            item->available_tokens = tokens;
            item->input_index = ATPERSON_CONTEXT_NO_INPUT_INDEX;
            item->observed_at = request->at_epoch;
            atp_context_copy_identifier(item->source_id, sizeof(item->source_id),
                                        request->source_id);
            atp_context_copy_identifier(item->author_did, sizeof(item->author_did),
                                        request->author_did);
        }
    }

    if (request->immediate_text && request->immediate_text[0] != '\0' &&
        selection_config.max_memory_items > 0u && selection_config.max_memory_scan > 0u) {
        const size_t episode_count = atp_graph_episode_count(graph);
        const size_t scan_count = episode_count < selection_config.max_memory_scan
                                      ? episode_count
                                      : selection_config.max_memory_scan;
        out->memory_episodes_scanned = scan_count;
        out->memory_scan_truncated = scan_count < episode_count;

        atp_recall_result recalled[ATPERSON_CONTEXT_MAX_MEMORY_ITEMS] = {0};
        size_t recalled_count = 0u;
        const atp_status recall_status = atp_graph_recall_ranked_preview(
            graph, request->immediate_text, request->at_epoch, selection_config.max_memory_scan,
            recalled, selection_config.max_memory_items, &recalled_count);
        if (recall_status != ATP_OK) {
            return recall_status;
        }

        for (size_t i = 0u; i < recalled_count; ++i) {
            atp_context_item *item = &candidates[candidate_count++];
            item->kind = ATP_CONTEXT_ITEM_MEMORY;
            item->reason = ATP_CONTEXT_REASON_EPISODIC_RECALL;
            item->score = atp_context_normalize_positive(recalled[i].score);
            item->available_tokens = recalled[i].episode.token_count;
            item->input_index = ATPERSON_CONTEXT_NO_INPUT_INDEX;
            item->observed_at = recalled[i].episode.observed_at;
            item->ledger_id = recalled[i].episode.ledger_id;
            item->recall = recalled[i];
            atp_context_copy_identifier(item->source_id, sizeof(item->source_id),
                                        recalled[i].episode.source_id);
            atp_context_copy_identifier(item->author_did, sizeof(item->author_did),
                                        recalled[i].episode.author_did);
        }
    }

    for (size_t i = 0u; i < request->recent_count; ++i) {
        out->recent_inputs_scored++;
        const atp_context_recent_input *recent = &request->recent[i];
        const size_t tokens = atp_context_count_tokens(recent->text);
        if (tokens == 0u) {
            continue;
        }

        atp_context_item *item = &candidates[candidate_count++];
        item->kind = ATP_CONTEXT_ITEM_RECENT;
        item->reason = ATP_CONTEXT_REASON_RECENT_INTERACTION;
        item->score = atp_context_recency(recent->observed_at, request->at_epoch);
        item->available_tokens = tokens;
        item->input_index = i;
        item->observed_at = recent->observed_at;
        atp_context_copy_identifier(item->source_id, sizeof(item->source_id), recent->source_id);
        atp_context_copy_identifier(item->author_did, sizeof(item->author_did), recent->author_did);
    }

    if (request->author_did && request->author_did[0] != '\0') {
        atp_interaction_state state = {0};
        const atp_status state_status = atp_graph_interaction_lookup(
            graph, ATP_INTERACTION_SUBJECT_AUTHOR, request->author_did, &state);
        if (state_status == ATP_OK) {
            atp_context_item *item = &candidates[candidate_count++];
            item->kind = ATP_CONTEXT_ITEM_AUTHOR_STATE;
            item->reason = ATP_CONTEXT_REASON_AUTHOR_FAMILIARITY;
            item->score = state.familiarity;
            item->input_index = ATPERSON_CONTEXT_NO_INPUT_INDEX;
            item->observed_at = state.last_seen_at;
            item->interaction = state;
            atp_context_copy_identifier(item->author_did, sizeof(item->author_did),
                                        state.identifier);
        } else if (state_status != ATP_ERR_NOT_FOUND) {
            return state_status;
        }
    }

    if (request->source_id && request->source_id[0] != '\0') {
        atp_interaction_state state = {0};
        const atp_status state_status = atp_graph_interaction_lookup(
            graph, ATP_INTERACTION_SUBJECT_SOURCE, request->source_id, &state);
        if (state_status == ATP_OK) {
            atp_context_item *item = &candidates[candidate_count++];
            item->kind = ATP_CONTEXT_ITEM_SOURCE_STATE;
            item->reason = ATP_CONTEXT_REASON_SOURCE_FAMILIARITY;
            item->score = state.familiarity;
            item->input_index = ATPERSON_CONTEXT_NO_INPUT_INDEX;
            item->observed_at = state.last_seen_at;
            item->interaction = state;
            atp_context_copy_identifier(item->source_id, sizeof(item->source_id),
                                        state.identifier);
        } else if (state_status != ATP_ERR_NOT_FOUND) {
            return state_status;
        }
    }

    if (candidate_count > 1u) {
        qsort(candidates, candidate_count, sizeof(*candidates), atp_context_item_compare);
    }

    size_t recent_selected = 0u;
    for (size_t i = 0u; i < candidate_count; ++i) {
        atp_context_item item = candidates[i];
        if (item.kind == ATP_CONTEXT_ITEM_RECENT &&
            recent_selected >= selection_config.max_recent_items) {
            continue;
        }

        if (out->item_count >= selection_config.max_items) {
            out->item_limit_reached = true;
            break;
        }

        if (item.available_tokens > 0u) {
            const size_t remaining = selection_config.max_tokens - out->token_count;
            if (remaining == 0u) {
                out->token_limit_reached = true;
                continue;
            }
            item.selected_tokens =
                item.available_tokens < remaining ? item.available_tokens : remaining;
            item.truncated = item.selected_tokens < item.available_tokens;
            if (item.truncated) {
                out->token_limit_reached = true;
            }
            out->token_count += item.selected_tokens;
        }

        out->items[out->item_count++] = item;
        if (item.kind == ATP_CONTEXT_ITEM_RECENT) {
            recent_selected++;
        }
    }

    return ATP_OK;
}
