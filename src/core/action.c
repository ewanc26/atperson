#include "atperson/action.h"
#include "internal.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool atp_action_is_token_byte(unsigned char byte) {
    return byte >= 0x80u || isalnum(byte) || byte == '\'' || byte == '-' || byte == '_';
}

static unsigned char atp_action_normalize_ascii(unsigned char byte) {
    return byte < 0x80u ? (unsigned char)tolower(byte) : byte;
}

static float atp_action_clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static int atp_action_candidate_compare(const void *left, const void *right) {
    const atp_action_candidate *a = left;
    const atp_action_candidate *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->association_score < b->association_score) {
        return 1;
    }
    if (a->association_score > b->association_score) {
        return -1;
    }
    if (a->familiarity_score < b->familiarity_score) {
        return 1;
    }
    if (a->familiarity_score > b->familiarity_score) {
        return -1;
    }
    if (a->supporting_observations < b->supporting_observations) {
        return 1;
    }
    if (a->supporting_observations > b->supporting_observations) {
        return -1;
    }
    return strcmp(a->token, b->token);
}

atp_status atp_graph_action_candidates(const atp_graph *graph, const char *context,
                                       atp_action_candidate *out, size_t capacity,
                                       size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !context || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (capacity == 0u || graph->node_count == 0u || graph->edge_count == 0u) {
        return ATP_OK;
    }

    bool *context_nodes = calloc(graph->node_count, sizeof(*context_nodes));
    if (!context_nodes) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t context_count = 0u;
    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;
    for (const unsigned char *cursor = (const unsigned char *)context;; ++cursor) {
        const unsigned char byte = *cursor;
        const bool token_byte = byte != '\0' && atp_action_is_token_byte(byte);
        if (token_byte) {
            if (token_len + 1u < sizeof(token)) {
                token[token_len++] = (char)atp_action_normalize_ascii(byte);
            }
        }
        if ((!token_byte || byte == '\0') && token_len > 0u) {
            token[token_len] = '\0';
            token_len = 0u;
            const int32_t node = atp_find_node(graph, token);
            if (node >= 0 && !context_nodes[node]) {
                context_nodes[node] = true;
                context_count++;
            }
        }
        if (byte == '\0') {
            break;
        }
    }

    if (context_count == 0u) {
        free(context_nodes);
        return ATP_OK;
    }

    float *association_sums = calloc(graph->node_count, sizeof(*association_sums));
    uint64_t *supporting_observations = calloc(graph->node_count, sizeof(*supporting_observations));
    uint32_t *context_matches = calloc(graph->node_count, sizeof(*context_matches));
    if (!association_sums || !supporting_observations || !context_matches) {
        free(context_nodes);
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0u; i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        if (edge->source >= graph->node_count || edge->target >= graph->node_count ||
            !context_nodes[edge->source]) {
            continue;
        }

        const float neural = atp_network_score(graph, edge->source, edge->target);
        const float association = atp_action_clamp01(edge->strength * 0.7f + neural * 0.3f);
        association_sums[edge->target] += association;
        supporting_observations[edge->target] += edge->observations;
        context_matches[edge->target]++;
    }
    free(context_nodes);

    size_t candidate_count = 0u;
    for (size_t i = 0u; i < graph->node_count; ++i) {
        if (context_matches[i] > 0u) {
            candidate_count++;
        }
    }
    if (candidate_count == 0u) {
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_OK;
    }

    atp_action_candidate *ranked = calloc(candidate_count, sizeof(*ranked));
    if (!ranked) {
        free(association_sums);
        free(supporting_observations);
        free(context_matches);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t next = 0u;
    for (size_t i = 0u; i < graph->node_count; ++i) {
        if (context_matches[i] == 0u) {
            continue;
        }

        const float association_score =
            atp_action_clamp01(association_sums[i] / (float)context_matches[i]);
        const float familiarity_score = atp_action_clamp01(
            graph->nodes[i].familiarity * (1.0f - graph->config.familiarity_decay));
        const uint64_t support = supporting_observations[i];
        const float support_score = support == UINT64_MAX
                                        ? 1.0f
                                        : (float)((double)support / ((double)support + 1.0));

        atp_action_candidate *candidate = &ranked[next++];
        strncpy(candidate->token, graph->nodes[i].token, sizeof(candidate->token) - 1u);
        candidate->association_score = association_score;
        candidate->familiarity_score = familiarity_score;
        candidate->support_score = support_score;
        candidate->score = (association_score + familiarity_score + support_score) / 3.0f;
        candidate->supporting_observations = support;
        candidate->context_matches = context_matches[i];
    }

    free(association_sums);
    free(supporting_observations);
    free(context_matches);

    qsort(ranked, candidate_count, sizeof(*ranked), atp_action_candidate_compare);
    const size_t written = candidate_count < capacity ? candidate_count : capacity;
    memcpy(out, ranked, written * sizeof(*out));
    free(ranked);

    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}
