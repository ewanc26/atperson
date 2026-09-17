#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Experience-derived valence (issue #13).
 *
 * State: one atp_valence_record per token that has received at least one
 * explicit event, kept sorted by node_index for binary search; plus a
 * bounded provenance log of the most recent events. Records are created
 * by events only — never by observation — so the empty-start invariant
 * holds and a rebuilt-without-events graph reads neutral.
 *
 * Update equation: valence' = valence + rate * (signal - valence). An EMA
 * of signals in [-1, 1] stays in [-1, 1]; no time-based decay, so the
 * score is exactly the folded evidence and contrary evidence reverses it.
 */

static const float ATP_VALENCE_DEFAULT_RATE = 0.25f;

static int64_t atp_valence_find(const atp_graph *graph, uint32_t node_index) {
    size_t low = 0u;
    size_t high = graph->valence_record_count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2u;
        const uint32_t mid_index = graph->valence_records[mid].node_index;
        if (mid_index == node_index) {
            return (int64_t)mid;
        }
        if (mid_index < node_index) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    return -(int64_t)low - 1; /* insertion point, negated and offset */
}

static bool atp_valence_reserve(atp_graph *graph, size_t needed) {
    if (needed <= graph->valence_record_capacity) {
        return true;
    }
    size_t capacity = graph->valence_record_capacity ? graph->valence_record_capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    atp_valence_record *grown = realloc(graph->valence_records, capacity * sizeof(*grown));
    if (!grown) {
        return false;
    }
    graph->valence_records = grown;
    graph->valence_record_capacity = capacity;
    return true;
}

static bool atp_valence_log_append(atp_graph *graph, uint32_t node_index, uint32_t kind,
                                   float signal, uint64_t at_epoch, const char *source_id) {
    if (!graph->valence_events) {
        graph->valence_events =
            malloc(ATPERSON_VALENCE_EVENT_CAPACITY * sizeof(*graph->valence_events));
        if (!graph->valence_events) {
            return false;
        }
        graph->valence_event_capacity = ATPERSON_VALENCE_EVENT_CAPACITY;
        graph->valence_event_count = 0u;
    }

    if (graph->valence_event_count == graph->valence_event_capacity) {
        memmove(graph->valence_events, graph->valence_events + 1u,
                (graph->valence_event_capacity - 1u) * sizeof(*graph->valence_events));
        graph->valence_event_count--;
        graph->valence_event_evictions++;
    }

    atp_valence_event_log_entry *entry = &graph->valence_events[graph->valence_event_count++];
    entry->node_index = node_index;
    entry->kind = kind;
    entry->signal = signal;
    entry->at_epoch = at_epoch;
    memset(entry->source_id, 0, sizeof(entry->source_id));
    if (source_id) {
        const size_t len = strlen(source_id);
        memcpy(entry->source_id, source_id,
               len < sizeof(entry->source_id) ? len : sizeof(entry->source_id) - 1u);
    }
    return true;
}

float atp_valence_effective_rate(const atp_graph *graph) {
    const float rate = graph->config.valence_rate;
    if (!(rate > 0.0f) || rate > 1.0f || !isfinite(rate)) {
        return ATP_VALENCE_DEFAULT_RATE;
    }
    return rate;
}

atp_status atp_graph_valence_event(atp_graph *graph, const char *token, atp_valence_kind kind,
                                   float signal, uint64_t at_epoch, const char *source_id) {
    if (!graph || !token) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (kind < ATP_VALENCE_ACTION || kind > ATP_VALENCE_AVOID) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (!isfinite(signal)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const int32_t node = atp_find_node(graph, token);
    if (node < 0) {
        /* Valence attaches to experienced subjects only. Interning
         * vocabulary from an event would let one event create learned
         * state, breaking the empty-start invariant. */
        return ATP_ERR_NOT_FOUND;
    }
    if (signal > 1.0f) {
        signal = 1.0f;
    } else if (signal < -1.0f) {
        signal = -1.0f;
    }

    const float rate = atp_valence_effective_rate(graph);
    const int64_t found = atp_valence_find(graph, (uint32_t)node);
    if (found >= 0) {
        atp_valence_record *record = &graph->valence_records[found];
        record->valence = record->valence + rate * (signal - record->valence);
        record->event_count++;
        if (signal > 0.0f) {
            record->positive_events++;
        } else if (signal < 0.0f) {
            record->negative_events++;
        }
        record->last_event_at = at_epoch;
    } else {
        const size_t insert = (size_t)(-found - 1);
        if (!atp_valence_reserve(graph, graph->valence_record_count + 1u)) {
            return ATP_ERR_OUT_OF_MEMORY;
        }
        memmove(graph->valence_records + insert + 1u, graph->valence_records + insert,
                (graph->valence_record_count - insert) * sizeof(*graph->valence_records));
        atp_valence_record *record = &graph->valence_records[insert];
        memset(record, 0, sizeof(*record));
        record->node_index = (uint32_t)node;
        record->valence = rate * signal; /* from neutral */
        record->event_count = 1u;
        if (signal > 0.0f) {
            record->positive_events = 1u;
        } else if (signal < 0.0f) {
            record->negative_events = 1u;
        }
        record->last_event_at = at_epoch;
        graph->valence_record_count++;
    }

    if (!atp_valence_log_append(graph, (uint32_t)node, (uint32_t)kind, signal, at_epoch,
                                source_id)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    return ATP_OK;
}

static void atp_valence_state_fill(const atp_graph *graph, const atp_valence_record *record,
                                   atp_valence_state *out) {
    memset(out, 0, sizeof(*out));
    const atp_node *node = &graph->nodes[record->node_index];
    const size_t len = strlen(node->token);
    memcpy(out->token, node->token, len < sizeof(out->token) ? len : sizeof(out->token) - 1u);
    out->valence = record->valence;
    out->event_count = record->event_count;
    out->positive_events = record->positive_events;
    out->negative_events = record->negative_events;
    out->last_event_at = record->last_event_at;
}

atp_status atp_graph_valence(const atp_graph *graph, const char *token, atp_valence_state *out) {
    if (!graph || !token || !out) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const int32_t node = atp_find_node(graph, token);
    if (node < 0) {
        return ATP_ERR_NOT_FOUND;
    }
    const int64_t found = atp_valence_find(graph, (uint32_t)node);
    if (found < 0) {
        return ATP_ERR_NOT_FOUND; /* known token, never valued */
    }
    atp_valence_state_fill(graph, &graph->valence_records[found], out);
    return ATP_OK;
}

atp_status atp_graph_valence_at(const atp_graph *graph, size_t index, atp_valence_state *out) {
    if (!graph || !out) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->valence_record_count) {
        return ATP_ERR_NOT_FOUND;
    }
    atp_valence_state_fill(graph, &graph->valence_records[index], out);
    return ATP_OK;
}

size_t atp_graph_valence_count(const atp_graph *graph) {
    return graph ? graph->valence_record_count : 0u;
}

atp_status atp_graph_valence_log(const atp_graph *graph, atp_valence_event *out, size_t capacity,
                                 size_t *out_count) {
    if (!graph || !out_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const size_t count =
        capacity < graph->valence_event_count ? capacity : graph->valence_event_count;
    for (size_t i = 0u; i < count; ++i) {
        /* Newest first: the log stores oldest first. */
        const atp_valence_event_log_entry *entry =
            &graph->valence_events[graph->valence_event_count - 1u - i];
        atp_valence_event *target = &out[i];
        memset(target, 0, sizeof(*target));
        target->kind = (atp_valence_kind)entry->kind;
        target->signal = entry->signal;
        target->at_epoch = entry->at_epoch;
        const atp_node *node = &graph->nodes[entry->node_index];
        const size_t token_len = strlen(node->token);
        memcpy(target->token, node->token,
               token_len < sizeof(target->token) ? token_len : sizeof(target->token) - 1u);
        const size_t source_len = strnlen(entry->source_id, sizeof(entry->source_id));
        memcpy(target->source_id, entry->source_id,
               source_len < sizeof(target->source_id) ? source_len
                                                      : sizeof(target->source_id) - 1u);
    }
    *out_count = count;
    return ATP_OK;
}

uint64_t atp_graph_valence_log_evictions(const atp_graph *graph) {
    return graph ? graph->valence_event_evictions : 0u;
}
