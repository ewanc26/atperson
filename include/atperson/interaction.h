#ifndef ATPERSON_INTERACTION_H
#define ATPERSON_INTERACTION_H

#include "atperson/core.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_INTERACTION_ID_BYTES ATPERSON_LEDGER_SOURCE_BYTES

typedef enum atp_interaction_subject {
    ATP_INTERACTION_SUBJECT_AUTHOR = 1,
    ATP_INTERACTION_SUBJECT_SOURCE = 2
} atp_interaction_subject;

/*
 * Neutral, experience-derived continuity for one author DID or source URI.
 *
 * This is deliberately not a relationship or trust model. The values are
 * derived on demand from authoritative state already persisted by the core:
 * LEARNED entries in the graph's ledger mirror and currently retained
 * episodic memories. There is no parallel mutable counter table to drift from
 * replay, compaction, withdrawal, or snapshot state.
 */
typedef struct atp_interaction_state {
    atp_interaction_subject subject;
    char identifier[ATPERSON_INTERACTION_ID_BYTES];
    uint64_t encounter_count;
    uint64_t last_seen_at;
    uint64_t remembered_episode_count;
    float familiarity;
} atp_interaction_state;

static inline float atp_interaction_familiarity(uint64_t encounter_count) {
    if (encounter_count == 0u) {
        return 0.0f;
    }
    if (encounter_count == UINT64_MAX) {
        return 1.0f;
    }
    return (float)((double)encounter_count / ((double)encounter_count + 1.0));
}

/**
 * Derive neutral interaction state for an author DID or source URI.
 *
 * Only LEARNED ledger-mirror entries count as encounters. SKIPPED records are
 * intentionally excluded so moderation/ingestion-policy decisions cannot
 * silently create learned familiarity. `last_seen_at` is the greatest known
 * observation timestamp among matching learned entries (0 when timestamps
 * were unavailable). `remembered_episode_count` counts matching episodes that
 * are still retained by episodic memory.
 *
 * `familiarity` is the bounded factual exposure transform:
 *
 *     encounter_count / (encounter_count + 1)
 *
 * It carries no trust, affinity, friendship, preference, or sentiment
 * meaning. Stable DIDs and AT URIs should be used; this layer performs no
 * handle resolution or network access.
 *
 * Unknown identifiers return ATP_ERR_NOT_FOUND and leave `out` zeroed.
 * Queries are deterministic and read-only. Withdrawal follows the existing
 * model: the live graph remains unchanged until rebuild; after rebuild,
 * withdrawn entries and their episodes no longer contribute here either.
 */
static inline atp_status atp_graph_interaction_lookup(const atp_graph *graph,
                                                       atp_interaction_subject subject,
                                                       const char *identifier,
                                                       atp_interaction_state *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!graph || !identifier || !out || identifier[0] == '\0' ||
        (subject != ATP_INTERACTION_SUBJECT_AUTHOR &&
         subject != ATP_INTERACTION_SUBJECT_SOURCE)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const size_t identifier_len = strlen(identifier);
    if (identifier_len >= sizeof(out->identifier)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    uint64_t encounters = 0u;
    uint64_t last_seen_at = 0u;
    const size_t ledger_count = atp_graph_ledger_count(graph);
    for (size_t i = 0u; i < ledger_count; ++i) {
        atp_ledger_entry entry = {0};
        const atp_status status = atp_graph_ledger_entry(graph, i, &entry);
        if (status != ATP_OK) {
            return status;
        }
        if (entry.outcome != ATP_LEDGER_OUTCOME_LEARNED) {
            continue;
        }

        const char *candidate = subject == ATP_INTERACTION_SUBJECT_AUTHOR
                                    ? entry.author_did
                                    : entry.source_id;
        if (candidate[0] == '\0' || strcmp(candidate, identifier) != 0) {
            continue;
        }

        if (encounters != UINT64_MAX) {
            encounters++;
        }
        if (entry.observed_at > last_seen_at) {
            last_seen_at = entry.observed_at;
        }
    }

    if (encounters == 0u) {
        return ATP_ERR_NOT_FOUND;
    }

    uint64_t remembered = 0u;
    const size_t episode_count = atp_graph_episode_count(graph);
    for (size_t i = 0u; i < episode_count; ++i) {
        atp_episode episode = {0};
        const atp_status status = atp_graph_episode_at(graph, i, &episode);
        if (status != ATP_OK) {
            return status;
        }
        const char *candidate = subject == ATP_INTERACTION_SUBJECT_AUTHOR
                                    ? episode.author_did
                                    : episode.source_id;
        if (candidate[0] != '\0' && strcmp(candidate, identifier) == 0 &&
            remembered != UINT64_MAX) {
            remembered++;
        }
    }

    out->subject = subject;
    memcpy(out->identifier, identifier, identifier_len + 1u);
    out->encounter_count = encounters;
    out->last_seen_at = last_seen_at;
    out->remembered_episode_count = remembered;
    out->familiarity = atp_interaction_familiarity(encounters);
    return ATP_OK;
}

#ifdef __cplusplus
}
#endif

#endif
