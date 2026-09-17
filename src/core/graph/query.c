#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct atp_recall_match {
    size_t episode_index;
    float score;
    uint64_t observed_at;
    uint64_t ledger_id;
} atp_recall_match;

static int atp_recall_match_compare(const void *left, const void *right) {
    const atp_recall_match *a = left;
    const atp_recall_match *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
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
    return 0;
}

/* Recall query walk: collects distinct known node indices. */
typedef struct atp_query_walk {
    const atp_graph *graph;
    uint32_t *nodes;
    size_t count;
    size_t capacity;
    atp_status status;
} atp_query_walk;

static bool atp_query_emit(void *userdata, const char *token) {
    atp_query_walk *walk = userdata;
    const int32_t node = atp_find_node(walk->graph, token);
    if (node < 0) {
        return true;
    }
    for (size_t i = 0u; i < walk->count; ++i) {
        if (walk->nodes[i] == (uint32_t)node) {
            return true;
        }
    }
    if (walk->count == walk->capacity) {
        const size_t next = walk->capacity ? walk->capacity * 2u : 8u;
        if (next < walk->count || next > SIZE_MAX / sizeof(*walk->nodes)) {
            walk->status = ATP_ERR_OUT_OF_MEMORY;
            return false;
        }
        uint32_t *grown = realloc(walk->nodes, next * sizeof(*grown));
        if (!grown) {
            walk->status = ATP_ERR_OUT_OF_MEMORY;
            return false;
        }
        walk->nodes = grown;
        walk->capacity = next;
    }
    walk->nodes[walk->count++] = (uint32_t)node;
    return true;
}

/* Shared query helper (src/core/graph.c): tokenize a query under the
 * current schema and collect distinct known node indices. Caller frees
 * *out_nodes. Unknown tokens are ignored; the graph is not mutated. */
atp_status atp_graph_query_nodes(const atp_graph *graph, const char *query, uint32_t **out_nodes,
                                 size_t *out_count) {
    if (out_nodes) {
        *out_nodes = NULL;
    }
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || !out_nodes || !out_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_query_walk walk = {
        .graph = graph,
    };
    atp_tokenize(query, ATPERSON_SCHEMA_VERSION, atp_query_emit, &walk);
    if (walk.status != ATP_OK) {
        free(walk.nodes);
        return walk.status;
    }
    *out_nodes = walk.nodes;
    *out_count = walk.count;
    return ATP_OK;
}

atp_recall_config atp_recall_default_config(void) {
    return (atp_recall_config){
        .min_overlap = 0.0f,
        .max_prefilter = 0u,
        .disable = false,
    };
}

atp_status atp_graph_recall(atp_graph *graph, const char *query, uint64_t at_epoch,
                            const atp_recall_config *config, atp_recall_report *report,
                            atp_episode *out, size_t capacity, size_t *out_count) {
    const atp_recall_config defaults = atp_recall_default_config();
    const atp_recall_config *policy = config ? config : &defaults;

    if (report) {
        memset(report, 0, sizeof(*report));
        report->episodes_total = graph ? graph->episode_count : 0u;
    }
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !query || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (policy->disable) {
        if (report) {
            report->gate = ATP_RECALL_GATE_DISABLED;
        }
        return ATP_OK;
    }
    if (capacity == 0u) {
        return ATP_OK;
    }
    if (graph->episode_count == 0u) {
        return ATP_OK;
    }

    /* Distinct query node indices; tokens unknown to the vocabulary cannot
     * match anything and do not mutate the graph. */
    uint32_t *query_nodes = NULL;
    size_t query_count = 0u;
    const atp_status queried = atp_graph_query_nodes(graph, query, &query_nodes, &query_count);
    if (queried != ATP_OK) {
        return queried;
    }

    if (query_count == 0u) {
        /* No known tokens: zero overlap with every group. */
        if (report) {
            report->groups_total = graph->group_count;
            report->groups_scanned = 0u;
        }
        free(query_nodes);
        return ATP_OK;
    }

    /* Prefilter bound: scan only the most recent episodes. Episodes are kept
     * in insertion order, so the most recent are the tail of the array. */
    size_t scan_start = 0u;
    if (policy->max_prefilter > 0u && policy->max_prefilter < graph->episode_count) {
        scan_start = graph->episode_count - policy->max_prefilter;
        if (report) {
            report->gate = ATP_RECALL_GATE_PREFILTER;
        }
    }
    const size_t scan_count = graph->episode_count - scan_start;
    if (report) {
        report->episodes_scanned = scan_count;
    }

    /* Group-index prefilter (issue #55): skip episodes whose group's union
     * token set has zero overlap with the query. Conservative by
     * construction -- a scoring episode's summary tokens are in its
     * group's union -- so results are identical to the linear scan. Falls
     * back to the linear scan when the index is absent or a surviving
     * group's token set is saturated. */
    uint8_t *scan_episode = NULL;
    if (graph->episode_group_ids && graph->group_count > 0u) {
        scan_episode = calloc(graph->episode_count, sizeof(*scan_episode));
        if (scan_episode) {
            size_t groups_scanned = 0u;
            for (size_t g = 0u; g < graph->group_count; ++g) {
                const atp_episode_group *group = &graph->groups[g];
                bool overlap = group->token_count == ATPERSON_GROUP_TOKEN_MAX;
                if (!overlap) {
                    for (uint32_t t = 0u; t < group->token_count && !overlap; ++t) {
                        for (size_t q = 0u; q < query_count; ++q) {
                            if (group->tokens[t] == query_nodes[q]) {
                                overlap = true;
                                break;
                            }
                        }
                    }
                }
                if (!overlap) {
                    continue;
                }
                groups_scanned++;
                for (size_t i = 0u; i < graph->episode_count; ++i) {
                    if (graph->episode_group_ids[i] == (uint32_t)g) {
                        scan_episode[i] = 1u;
                    }
                }
            }
            if (report) {
                report->groups_total = graph->group_count;
                report->groups_scanned = groups_scanned;
            }
        }
    }

    atp_recall_match *matches = malloc(scan_count * sizeof(*matches));
    if (!matches) {
        free(scan_episode);
        free(query_nodes);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    size_t match_count = 0u;
    for (size_t i = scan_start; i < graph->episode_count; ++i) {
        if (scan_episode && !scan_episode[i]) {
            continue;
        }
        const atp_episode *episode = &graph->episodes[i];
        float score = 0.0f;
        for (uint32_t s = 0u; s < episode->token_count; ++s) {
            const uint32_t node_index = episode->summary[s].node_index;
            if (node_index >= graph->node_count) {
                continue;
            }
            for (size_t q = 0u; q < query_count; ++q) {
                if (query_nodes[q] == node_index) {
                    score += episode->summary[s].weight;
                    break;
                }
            }
        }
        /* Base eligibility is a nonzero overlap (the historical behaviour);
         * a configured gate additionally requires the minimum overlap. */
        if (score > 0.0f && score >= policy->min_overlap) {
            matches[match_count++] = (atp_recall_match){
                .episode_index = i,
                .score = score,
                .observed_at = episode->observed_at,
                .ledger_id = episode->ledger_id,
            };
        }
    }
    free(query_nodes);
    free(scan_episode);
    if (report) {
        report->episodes_matched = match_count;
        if (policy->min_overlap > 0.0f) {
            report->gate = ATP_RECALL_GATE_MIN_OVERLAP;
        }
    }

    qsort(matches, match_count, sizeof(*matches), atp_recall_match_compare);
    const size_t written = match_count < capacity ? match_count : capacity;
    for (size_t i = 0u; i < written; ++i) {
        atp_episode *episode = &graph->episodes[matches[i].episode_index];
        if (out) {
            out[i] = *episode;
        }
        episode->recall_count++;
        episode->last_recall_at = at_epoch;
    }
    free(matches);
    if (report) {
        report->episodes_returned = written;
    }
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}

size_t atp_graph_episode_count(const atp_graph *graph) {
    return graph ? graph->episode_count : 0u;
}

atp_status atp_graph_episode_at(const atp_graph *graph, size_t index, atp_episode *out_episode) {
    if (!graph || !out_episode) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->episode_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_episode = graph->episodes[index];
    return ATP_OK;
}

float atp_graph_familiarity(const atp_graph *graph, const char *token) {
    if (!graph || !token) {
        return 0.0f;
    }
    const int32_t node = atp_find_node(graph, token);
    if (node < 0) {
        return 0.0f;
    }
    return graph->nodes[node].familiarity;
}

bool atp_graph_has_token(const atp_graph *graph, const char *token) {
    if (!graph || !token) {
        return false;
    }
    return atp_find_node(graph, token) >= 0;
}

typedef struct atp_ranked_edge {
    const atp_edge *edge;
    float score;
} atp_ranked_edge;

static int atp_ranked_edge_compare(const void *left, const void *right) {
    const atp_ranked_edge *a = left;
    const atp_ranked_edge *b = right;
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    return 0;
}

/* Single-token lookup normalization. The input is one token, not a
 * sentence, so the whole normalized output is the token — but a caller
 * passing punctuation must not match vocabulary, so only token bytes are
 * kept, same rule as the scanner. */
typedef struct atp_lookup_walk {
    char output[ATPERSON_TOKEN_BYTES];
    size_t len;
} atp_lookup_walk;

static bool atp_lookup_emit(void *userdata, const char *token) {
    atp_lookup_walk *walk = userdata;
    walk->len = strlen(token);
    if (walk->len >= sizeof(walk->output)) {
        walk->len = sizeof(walk->output) - 1u;
    }
    memcpy(walk->output, token, walk->len);
    walk->output[walk->len] = '\0';
    /* First token wins; a lookup input never spans multiple tokens. */
    return false;
}

static void atp_normalize_lookup(const char *input, char output[ATPERSON_TOKEN_BYTES]) {
    atp_lookup_walk walk = {.len = 0u};
    atp_tokenize(input, ATPERSON_SCHEMA_VERSION, atp_lookup_emit, &walk);
    if (walk.len == 0u) {
        output[0] = '\0';
        return;
    }
    memcpy(output, walk.output, walk.len + 1u);
}

atp_status atp_graph_associations(const atp_graph *graph, const char *token, atp_association *out,
                                  size_t capacity, size_t *out_count) {
    if (out_count) {
        *out_count = 0u;
    }
    if (!graph || !token || (!out && capacity > 0u)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    char normalized[ATPERSON_TOKEN_BYTES];
    atp_normalize_lookup(token, normalized);
    const int32_t source = atp_find_node(graph, normalized);
    if (source < 0) {
        return ATP_ERR_NOT_FOUND;
    }

    size_t candidate_count = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        if (graph->edges[i].source == (uint32_t)source) {
            candidate_count++;
        }
    }
    if (candidate_count == 0u || capacity == 0u) {
        return ATP_OK;
    }

    atp_ranked_edge *ranked = malloc(candidate_count * sizeof(*ranked));
    if (!ranked) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    size_t next = 0u;
    for (size_t i = 0; i < graph->edge_count; ++i) {
        const atp_edge *edge = &graph->edges[i];
        if (edge->source != (uint32_t)source) {
            continue;
        }
        const float neural = atp_network_score(graph, edge->source, edge->target);
        ranked[next++] = (atp_ranked_edge){
            .edge = edge,
            .score = edge->strength * 0.7f + neural * 0.3f,
        };
    }

    qsort(ranked, candidate_count, sizeof(*ranked), atp_ranked_edge_compare);
    const size_t written = candidate_count < capacity ? candidate_count : capacity;
    for (size_t i = 0; i < written; ++i) {
        const atp_edge *edge = ranked[i].edge;
        const atp_node *target = &graph->nodes[edge->target];
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].token, target->token, sizeof(out[i].token) - 1u);
        out[i].score = ranked[i].score;
        out[i].observations = edge->observations;
        out[i].last_source_hash = edge->last_source_hash;
    }

    free(ranked);
    if (out_count) {
        *out_count = written;
    }
    return ATP_OK;
}
