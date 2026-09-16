#ifndef ATPERSON_MEMORY_H
#define ATPERSON_MEMORY_H

#include "atperson/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Inspectable episodic-recall scoring.
 *
 * Exact overlap remains the dominant signal. Direct learned graph association
 * can make an episode eligible without literal overlap; familiarity, recency
 * and prior recall use are bounded secondary signals and can never make an
 * otherwise unrelated episode eligible by themselves.
 */
#define ATPERSON_RECALL_EXACT_WEIGHT 1.0f
#define ATPERSON_RECALL_ASSOCIATION_WEIGHT 0.50f
#define ATPERSON_RECALL_FAMILIARITY_WEIGHT 0.10f
#define ATPERSON_RECALL_RECENCY_WEIGHT 0.10f
#define ATPERSON_RECALL_USE_WEIGHT 0.05f
#define ATPERSON_RECALL_RECENCY_WINDOW_SECONDS UINT64_C(86400)

typedef struct atp_recall_result {
    atp_episode episode;
    float score;
    float exact_score;
    float association_score;
    float familiarity_score;
    float recency_score;
    float use_score;
    uint32_t exact_token_matches;
    uint32_t association_token_matches;
} atp_recall_result;

/**
 * Recall episodes using inspectable lexical and learned-association evidence.
 *
 * Components:
 * - exact_score: raw weighted summary-token overlap with the query;
 * - association_score: raw weighted direct graph association support for
 *   otherwise-unmatched summary tokens, using the strongest learned edge in
 *   either direction between a query token and a summary token;
 * - familiarity_score: weighted mean bounded token exposure for summary tokens
 *   supported by exact or association evidence;
 * - recency_score: 1 / (1 + age / one day), or 0 when timestamps are unknown;
 * - use_score: recall_count / (recall_count + 1).
 *
 * Total score is the documented weighted sum of those five components using
 * ATPERSON_RECALL_*_WEIGHT. An episode is eligible only when exact_score > 0
 * or association_score > 0. Familiarity, recency and use therefore cannot
 * recall unrelated material by themselves.
 *
 * Results are deterministic for fixed graph state and inputs. Ties are broken
 * by exact support, association support, observation recency, then ledger id.
 * Returned episode copies reflect recall counters before this call; the stored
 * episodes selected for return then increment recall_count and set
 * last_recall_at to at_epoch, matching atp_graph_recall semantics.
 *
 * Unknown query tokens never enter the vocabulary. capacity == 0 performs no
 * recall and no mutation. Source/author/ledger provenance remains in the
 * embedded episode.
 */
atp_status atp_graph_recall_ranked(atp_graph *graph, const char *query, uint64_t at_epoch,
                                   atp_recall_result *out, size_t capacity,
                                   size_t *out_count);

/**
 * Read-only ranked-recall preview for planning/context selection.
 *
 * The scoring and ordering are identical to atp_graph_recall_ranked, but no
 * recall counters or timestamps are updated. `episode_scan_limit` is an
 * explicit work bound: at most that many of the most recently retained
 * episodes (the tail of the deterministic episode store) are examined.
 * A limit of 0 returns no results. `capacity` still limits only copied ranked
 * results within that scanned window.
 *
 * This API exists so merely considering a memory for a plan cannot itself
 * become learned/usage state. Query tokenisation uses the current shared
 * schema-versioned tokenizer and never interns unknown tokens.
 */
atp_status atp_graph_recall_ranked_preview(const atp_graph *graph, const char *query,
                                           uint64_t at_epoch, size_t episode_scan_limit,
                                           atp_recall_result *out, size_t capacity,
                                           size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
