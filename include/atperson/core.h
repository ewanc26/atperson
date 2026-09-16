#ifndef ATPERSON_CORE_H
#define ATPERSON_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_EMBEDDING_DIM 16u
#define ATPERSON_TOKEN_BYTES 96u

/*
 * Snapshot format version. Version 2 adds the mirrored observation ledger
 * block to the original version 1 layout. Snapshot files remain host-oriented
 * (fixed-width integers with host byte order), following the documented plan
 * that a future portable format defines byte order before snapshots become a
 * long-term interchange format.
 */
#define ATPERSON_SNAPSHOT_VERSION 2u

/*
 * Observation ledger format version. The ledger keeps each observation in an
 * append-only record log plus a durable commit marker (the fsync'd offset)
 * that is rewritten through a temporary file and rename so a crash can never
 * observe a partially updated commit marker.
 */
#define ATPERSON_LEDGER_VERSION 1u

/*
 * Learning schema version attached to each ledger entry. Bump this when the
 * graph's learning algorithm changes so a rebuild-from-ledger replay can
 * reject entries recorded under an incompatible schema.
 */
#define ATPERSON_SCHEMA_VERSION 1u

#define ATPERSON_LEDGER_SOURCE_BYTES 256u
#define ATPERSON_LEDGER_AUTHOR_BYTES 256u

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
} atp_graph_config;

typedef struct atp_graph_stats {
    size_t node_count;
    size_t edge_count;
    uint64_t observations;
    uint64_t token_observations;
    uint64_t training_steps;
    double mean_loss;
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
 * Real failures (I/O, invalid arguments) set `status` to a non-OK value.
 * `author_did` may be NULL or empty. `observed_at` is Unix epoch seconds,
 * 0 when unknown. Appending is durable before this function returns.
 */
atp_ledger_result atp_ledger_append(atp_ledger *ledger, const char *source_id,
                                    const char *author_did, uint64_t observed_at,
                                    uint64_t content_digest, uint32_t schema_version,
                                    atp_ledger_outcome outcome, uint64_t *out_id,
                                    atp_status *status);

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

#ifdef __cplusplus
}
#endif

#endif
