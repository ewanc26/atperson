#ifndef ATPERSON_CORE_H
#define ATPERSON_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_EMBEDDING_DIM 16u
#define ATPERSON_TOKEN_BYTES 96u

/*
 * Snapshot format version. Version 2 added the mirrored observation ledger
 * block; version 3 adds the episodic-memory block (a selective, consolidated
 * view of remembered observations with recall counters); version 4 adds the
 * per-token familiarity block (an exponentially weighted exposure score).
 * Snapshot files remain host-oriented (fixed-width integers with host byte
 * order), following the documented plan that a future portable format defines
 * byte order before snapshots become a long-term interchange format.
 */
#define ATPERSON_SNAPSHOT_VERSION 4u

/*
 * Observation ledger format version. The ledger keeps each observation in an
 * append-only record log plus a durable commit marker (the fsync'd offset)
 * that is rewritten through a temporary file and rename so a crash can never
 * observe a partially updated commit marker.
 *
 * v2 retains the canonical observation bytes inline in each entry record so
 * the ledger is replayable: a committed learnable entry can return the exact
 * bytes originally supplied to the learning core. v1 logs are migrated on
 * open (validated, transformed record-by-record to a temp file, fsync'd,
 * renamed); v1 entries carry no payload, which is reported honestly rather
 * than faked.
 */
#define ATPERSON_LEDGER_VERSION 2u

/*
 * Largest payload retained per ledger entry. Bounded so recovery parsing of
 * corrupted data stays bounded; larger observations are rejected at append
 * time rather than truncated silently.
 */
#define ATPERSON_LEDGER_PAYLOAD_LIMIT 65536u

/*
 * Learning schema version attached to each ledger entry. Bump this when the
 * graph's learning algorithm changes so a rebuild-from-ledger replay can
 * reject entries recorded under an incompatible schema.
 */
#define ATPERSON_SCHEMA_VERSION 1u

#define ATPERSON_LEDGER_SOURCE_BYTES 256u
#define ATPERSON_LEDGER_AUTHOR_BYTES 256u

/*
 * Episodic memory.
 *
 * An episode is a remembered observation. The ledger keeps every observation
 * durably; memory keeps a selective, consolidated view: the observation's
 * source (linked to the ledger entry it came from), and a compact summary of
 * the most significant tokens present. Recall is a use-based counter, not a
 * latent hidden state, so every memory change is inspectable and serialisable.
 */
#define ATPERSON_EPISODE_SUMMARY_SIZE 8u
#define ATPERSON_EPISODE_DEFAULT_CAPACITY 4096u

typedef enum atp_status {
    ATP_OK = 0,
    ATP_ERR_INVALID_ARGUMENT = 1,
    ATP_ERR_OUT_OF_MEMORY = 2,
    ATP_ERR_IO = 3,
    ATP_ERR_FORMAT = 4,
    ATP_ERR_NOT_FOUND = 5
} atp_status;

typedef struct atp_graph atp_graph;
typedef struct atp_ledger atp_ledger;

typedef struct atp_graph_config {
    uint64_t seed;
    float learning_rate;
    /*
     * Exponential decay for per-token familiarity, in [0, 1). Each exposure of
     * a token updates its score to `familiarity * familiarity_decay + 1`, so
     * repeated exposure slowly raises the score toward 1 / (1 - decay). 0
     * selects the default.
     */
    float familiarity_decay;
    /* Episode capacity (0 selects ATPERSON_EPISODE_DEFAULT_CAPACITY). */
    size_t episode_capacity;
} atp_graph_config;

typedef struct atp_graph_stats {
    size_t node_count;
    size_t edge_count;
    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double mean_loss;
    size_t episode_count;
    size_t episode_capacity;
    uint64_t episode_evictions;
} atp_graph_stats;

typedef struct atp_association {
    char token[ATPERSON_TOKEN_BYTES];
    float score;
    uint64_t observations;
    uint64_t last_source_hash;
} atp_association;

/*
 * Processing state of one ledger entry. PENDING entries were durably reserved
 * but not yet trained on (a crash between reservation and completion); FAILED
 * entries were examined but could not be trained and are retryable; LEARNED
 * and SKIPPED entries are committed and are never re-trained.
 */
typedef enum atp_ledger_outcome {
    ATP_LEDGER_OUTCOME_PENDING = 0,
    ATP_LEDGER_OUTCOME_LEARNED = 1,
    ATP_LEDGER_OUTCOME_SKIPPED = 2,
    ATP_LEDGER_OUTCOME_FAILED = 3
} atp_ledger_outcome;

/*
 * Result of an append or lookup against the unique (source id + digest)
 * index. A non-PENDING entry blocks re-append, so a restarted process can
 * never re-train on the same observation.
 */
typedef enum atp_ledger_result {
    ATP_LEDGER_NOT_FOUND = 0,
    ATP_LEDGER_NEW = 1,
    ATP_LEDGER_EXISTS_PENDING = 2,
    ATP_LEDGER_EXISTS_COMMITTED = 3
} atp_ledger_result;

typedef struct atp_ledger_entry {
    uint64_t id;
    uint64_t observed_at;
    uint64_t content_digest;
    uint32_t schema_version;
    atp_ledger_outcome outcome;
    char source_id[ATPERSON_LEDGER_SOURCE_BYTES];
    char author_did[ATPERSON_LEDGER_AUTHOR_BYTES];
} atp_ledger_entry;

/**
 * Return a stable default configuration. The graph starts with no vocabulary,
 * no edges, and randomly initialised neural parameters derived from `seed`.
 */
atp_graph_config atp_graph_default_config(void);

/** Create an empty language graph. */
atp_graph *atp_graph_create(const atp_graph_config *config);

/** Release a graph returned by atp_graph_create/atp_graph_load. */
void atp_graph_destroy(atp_graph *graph);

/**
 * Learn from one textual observation.
 *
 * `source_id` should be a stable provenance identifier such as an AT URI.
 * atperson hashes it into learned edges; it is never interpreted by the core.
 */
atp_status atp_graph_observe_text(atp_graph *graph, const char *text, const char *source_id);

/** Read aggregate graph/training statistics. */
atp_graph_stats atp_graph_get_stats(const atp_graph *graph);

/**
 * Return the strongest outgoing associations for `token`.
 *
 * Results are ordered strongest-first. `out_count` is always set when non-null.
 */
atp_status atp_graph_associations(const atp_graph *graph, const char *token, atp_association *out,
                                  size_t capacity, size_t *out_count);

/** Save the complete mutable learning state to a versioned binary snapshot. */
atp_status atp_graph_save(const atp_graph *graph, const char *path);

/** Load a graph snapshot. Returns NULL and writes status on failure. */
atp_graph *atp_graph_load(const char *path, atp_status *status);

const char *atp_status_string(atp_status status);

/*
 * Observation ledger.
 *
 * The ledger is the durable, append-only record of every observation fed to
 * the learning core. Records are immutable once committed; a processing
 * outcome change appends a small patch record, so the log stays append-only.
 *
 * Correctness relies on write ordering: the record bytes are written and
 * fsync'd to the log first, then the committed offset/count in `<path>.off`
 * is written to a temporary file, fsync'd, and renamed into place. A crash at
 * any point leaves the previous committed prefix intact; recovery truncates
 * any torn tail beyond the committed offset and heals a missing marker.
 *
 * `path` is the record log file. A sidecar `<path>.off` holds the durable
 * commit marker. The directory of `path` must already exist.
 */

/**
 * Open (creating if necessary) the ledger at `path`.
 * Returns NULL and writes `status` on failure; the caller must not have
 * created the directory hierarchy; that is the runtime's job.
 */
atp_ledger *atp_ledger_open(const char *path, atp_status *status);

/** Flush and close a ledger handle. NULL is a no-op. */
void atp_ledger_destroy(atp_ledger *ledger);

/**
 * Stable content digest over arbitrary bytes. This is what callers pass as
 * `content_digest`; it is the same digest a restarted process recomputes,
 * which is what makes the unique key cross-run stable.
 */
uint64_t atp_ledger_digest(const void *data, size_t length);

/**
 * Record one observation under the unique key (source_id, content_digest),
 * enforcing the dedup index:
 *
 *   - ATP_LEDGER_NEW            the entry was appended; `out_id` is set.
 *   - ATP_LEDGER_EXISTS_PENDING the key already exists but was not committed
 *                               (PENDING/FAILED); `out_id` is the existing
 *                               id and nothing new is appended.
 *   - ATP_LEDGER_EXISTS_COMMITTED the key already committed (LEARNED or
 *                               SKIPPED); `out_id` is the existing id and
 *                               nothing new is appended.
 *
 * `payload`/`payload_len` retain the canonical observation bytes inline in
 * the entry record (v2). Payloads larger than
 * ATPERSON_LEDGER_PAYLOAD_LIMIT are rejected. An empty payload (NULL/0) is
 * representable: it is the honest shape for observations with no text and
 * for v1-migrated entries whose bytes were never retained.
 *
 * Real failures (I/O, invalid arguments) set `status` to a non-OK value.
 * `author_did` may be NULL or empty. `observed_at` is Unix epoch seconds,
 * 0 when unknown. Appending is durable before this function returns.
 */
atp_ledger_result atp_ledger_append(atp_ledger *ledger, const char *source_id,
                                    const char *author_did, uint64_t observed_at,
                                    uint64_t content_digest, uint32_t schema_version,
                                    atp_ledger_outcome outcome, const void *payload,
                                    size_t payload_len, uint64_t *out_id, atp_status *status);

/**
 * Change the outcome of an existing entry via an appended patch record.
 * Closing a PENDING/FAILED entry to LEARNED/SKIPPED is the normal completion
 * path. Setting a committed entry back to PENDING is rejected because it
 * would reopen an already-trained observation; changing it to another
 * committed outcome is allowed.
 */
atp_status atp_ledger_set_outcome(atp_ledger *ledger, uint64_t id, atp_ledger_outcome outcome);

/** Dedup query on the unique (source id + digest) index. */
atp_ledger_result atp_ledger_lookup(const atp_ledger *ledger, const char *source_id,
                                    uint64_t content_digest, atp_ledger_entry *out_entry);

/** Number of entries currently committed to the ledger. */
uint64_t atp_ledger_count(const atp_ledger *ledger);

/** Copy the i-th committed entry (0-based, in append order). */
atp_status atp_ledger_entry_at(const atp_ledger *ledger, size_t index, atp_ledger_entry *out_entry);

/**
 * Copy the retained payload of entry `id` into `out` (at most `capacity`
 * bytes) and set `*out_len` to the payload length.
 *
 * The payload is re-verified against the entry's content digest on read:
 * corruption or a mismatching payload is reported as ATP_ERR_FORMAT rather
 * than returned as data. Entries with no retained payload (v1-migrated, or
 * appended empty) set `*out_len` to 0 and return ATP_OK.
 *
 * `out` may be NULL to query the required length alone.
 */
atp_status atp_ledger_entry_payload(const atp_ledger *ledger, uint64_t id, void *out,
                                     size_t capacity, size_t *out_len);

/*
 * Graph-side ledger mirror.
 *
 * The graph snapshot stores a versioned mirror of the ledger entries the
 * graph was trained from, so state can be rebuilt from either the ledger or
 * the snapshot alone (rebuild-from-ledger friendly deletion model).
 */

/** Record one ledger entry into the graph's mirror (copied by value). */
atp_status atp_graph_add_ledger_entry(atp_graph *graph, const atp_ledger_entry *entry);

/** Number of entries mirrored in the graph. */
size_t atp_graph_ledger_count(const atp_graph *graph);

/** Copy the i-th mirrored entry (0-based, in append order). */
atp_status atp_graph_ledger_entry(const atp_graph *graph, size_t index,
                                  atp_ledger_entry *out_entry);

/*
 * Episodic memory.
 *
 * Memory is a curated facet of the ledger: episodes are remembered (not every
 * observation), each stores the source link and a consolidated top-token
 * summary, and recall counters make consolidation visible. The weighted
 * association edges in the graph are the semantic substrate; episodes are the
 * episodic substrate.
 */

/** One summary token in an episode: the graph's node index and its weight. */
typedef struct atp_episode_token {
    uint32_t node_index;
    float weight;
} atp_episode_token;

/**
 * A remembered observation. `ledger_id` links the episode to its source entry
 * in the observation ledger; `recall_count`/`last_recall_at` are the usage
 * counters that drive eviction.
 */
typedef struct atp_episode {
    uint64_t ledger_id;
    uint64_t observed_at;
    uint64_t content_digest;
    uint32_t schema_version;
    uint64_t recall_count;
    uint64_t last_recall_at;
    uint32_t token_count;
    atp_episode_token summary[ATPERSON_EPISODE_SUMMARY_SIZE];
    char source_id[ATPERSON_LEDGER_SOURCE_BYTES];
    char author_did[ATPERSON_LEDGER_AUTHOR_BYTES];
} atp_episode;

/**
 * Learn from one textual observation and decide whether to remember it as an
 * episode (a single tokenisation pass).
 *
 * Selection is purely counter-based and inspectable: the episode is remembered
 * when the observation introduced new vocabulary or contained at least two
 * distinct tokens. `*out_remembered` is written after training. When the
 * memory is full, the least-recalled episode (then oldest) is evicted.
 */
atp_status atp_graph_observe_with_memory(atp_graph *graph, const char *text, const char *source_id,
                                         const char *author_did, uint64_t observed_at,
                                         uint64_t content_digest, uint32_t schema_version,
                                         uint64_t ledger_id, bool *out_remembered);

/**
 * Recall the episodes whose summary tokens overlap the query, strongest
 * overlap first (ties broken by recency, then ledger id). Recalled episodes
 * get their `recall_count` incremented and `last_recall_at` set to `at_epoch`;
 * query tokens are matched without mutating the vocabulary.
 */
atp_status atp_graph_recall(atp_graph *graph, const char *query, uint64_t at_epoch,
                            atp_episode *out, size_t capacity, size_t *out_count);

/** Number of episodes currently in memory. */
size_t atp_graph_episode_count(const atp_graph *graph);

/** Copy the i-th remembered episode (0-based, in insertion order). */
atp_status atp_graph_episode_at(const atp_graph *graph, size_t index, atp_episode *out_episode);

/*
 * Internal state.
 *
 * A slowly learned, experience-derived score per token that reflects repeated
 * exposure (an exponentially weighted count). Pure state, no value judgment:
 * it increases monotonically with how often a token has been seen and decays
 * toward zero as exposure stops.
 */

/**
 * Return the familiarity score for `token`; 0.0 when the token is unknown or
 * arguments are invalid. Querying never mutates the graph.
 */
float atp_graph_familiarity(const atp_graph *graph, const char *token);

#ifdef __cplusplus
}
#endif

#endif
