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
 * Version 5 is the portable format: little-endian integers, IEEE 754 float
 * bit patterns, framed sections (tag u32le | length u64le | payload) with
 * bounds-checked lengths and skippable unknown tags, and a trailing FNV-1a
 * digest. v4 snapshots load portably (every v4 writer in practice ran on a
 * little-endian host) and migrate to v5 on the next save; v1-v3 are refused.
 * Version 5 also persists the episode eviction counter, which v4 omitted.
 */
#define ATPERSON_SNAPSHOT_VERSION 5u

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
 * Learning schema version attached to each ledger entry. The schema version
 * identifies the learning behaviour an observation was recorded under:
 * tokenisation, negative sampling, memory selection, familiarity, and the
 * training equations. Bump it whenever any of those change.
 *
 * Schema history:
 * - 1: byte-oriented tokenisation (ASCII lowercase, bytes >= 0x80 pass
 *   through, silent truncation at 95 bytes).
 * - 2: Unicode tokenization contract (issue #8) — invalid UTF-8 sanitizes
 *   to U+FFFD, NFKC_Casefold + LUMP normalization, category-based token
 *   boundaries, codepoint-boundary truncation. Emoji are separators;
 *   combining marks are token bytes. See src/core/tokenize.c.
 *
 * Compatibility classes (see atp_schema_can_replay):
 * - Replay-compatible: replaying an entry under the new code reproduces the
 *   same learning effect as the old code did. The table lists the old
 *   version as replayable; historical ledgers keep working unchanged.
 * - Adapter migration: replay semantics differ but a versioned handler can
 *   reproduce the old behaviour. Add the old version to the table with an
 *   adapter entry point; replay dispatches on the entry's schema version.
 * - Incompatible: no handler can honestly reproduce the old behaviour (the
 *   training input itself changed meaning). The version is absent from the
 *   table; replay fails with ATP_ERR_SCHEMA naming the entry. Start a new
 *   model generation: a fresh ledger (or a compacted one, #7) under the new
 *   schema. Old and new algorithms are never ambiguously mixed.
 *
 * Mixed-schema ledgers accumulate across upgrades; replay processes them
 * deterministically in id order and fails at the first entry whose schema
 * is not replayable by this core.
 */
#define ATPERSON_SCHEMA_VERSION 2u

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
    ATP_ERR_NOT_FOUND = 5,
    /** A ledger entry or snapshot was recorded under a learning schema this
     * core cannot replay. Distinct from ATP_ERR_FORMAT (corruption): the
     * data is intact, the algorithm is the mismatch. */
    ATP_ERR_SCHEMA = 6
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
 * and SKIPPED entries are committed and are never re-trained. WITHDRAWN
 * entries are durably excluded by an operator or by source deletion: they
 * are never trained on, never mirrored, and excluded from rebuilds — the
 * rebuilt state is what the entity would have been without them. Withdrawal
 * is append-only (a patch record) and idempotent.
 */
typedef enum atp_ledger_outcome {
    ATP_LEDGER_OUTCOME_PENDING = 0,
    ATP_LEDGER_OUTCOME_LEARNED = 1,
    ATP_LEDGER_OUTCOME_SKIPPED = 2,
    ATP_LEDGER_OUTCOME_FAILED = 3,
    ATP_LEDGER_OUTCOME_WITHDRAWN = 4
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

/**
 * Durably withdraw one entry: an append-only patch record sets its outcome
 * to WITHDRAWN. Idempotent — withdrawing an already-withdrawn entry is a
 * no-op. Withdrawal never rewrites log history; the patch sequence on disk
 * is the audit trail. The live graph is not modified: withdrawn
 * contributions leave learned state at the next rebuild, which excludes
 * them entirely.
 */
atp_status atp_ledger_withdraw(atp_ledger *ledger, uint64_t id);

/**
 * Withdraw every entry whose source id matches `source_id` (e.g. one AT
 * URI). Returns the number of entries withdrawn; entries already WITHDRAWN
 * are not counted. An edited record (same URI, new content) appends a fresh
 * entry under the dedup index, so withdrawing the old content does not
 * block the new content from being observed.
 */
size_t atp_ledger_withdraw_source(atp_ledger *ledger, const char *source_id);

/**
 * Withdraw every entry whose author DID matches `author_did` — exclude an
 * account's contributions entirely. Returns the number of entries
 * withdrawn; already-withdrawn entries are not counted.
 */
size_t atp_ledger_withdraw_author(atp_ledger *ledger, const char *author_did);

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

/** What a compaction pass did, for logging and operator confidence. */
typedef struct atp_compact_report {
    uint64_t entries;           /**< entries written to the compacted log */
    uint64_t patches_flattened; /**< patch records folded into final outcomes */
    uint64_t payloads_dropped;  /**< WITHDRAWN payloads discarded */
    uint64_t bytes_before;      /**< committed log size before compaction */
    uint64_t bytes_after;       /**< committed log size after compaction */
} atp_compact_report;

/**
 * Rewrite the ledger into a compacted generation, atomically.
 *
 * The log grows without bound by design: append-only entries plus outcome
 * patches plus retained payloads. Compaction reclaims the provably dead
 * bytes while preserving every semantic the model depends on:
 *
 *   - Entry ids are stable. Episodes and source references need no
 *     remapping; the i-th entry keeps its id across generations.
 *   - Patch history flattens to final entry outcomes — the same documented
 *     behaviour as the v1 migration. The audit trail of *what* was
 *     observed is the entry set; per-patch history is not retained.
 *   - WITHDRAWN payloads are dropped: replay excludes withdrawn entries,
 *     the dedup index still suppresses re-observation, and withdrawal is
 *     durable, so the bytes are unreachable by design. The entry itself
 *     (source, digest, outcome) survives as a tombstone.
 *   - LEARNED/SKIPPED payloads are retained: replay needs LEARNED bytes,
 *     and PENDING/FAILED entries can still legally close to LEARNED.
 *   - Dedup behaviour is unchanged: the unique (source id + digest) index
 *     is rebuilt from the compacted log on reopen.
 *
 * Crash safety follows the same ordering as every other ledger write:
 * the compacted log is streamed to a temporary file and fsync'd first;
 * only then is the commit marker removed and the temp file renamed over
 * the original. A crash at any point leaves either the intact previous
 * log or the complete compacted log — recovery heals the marker from
 * whichever is present, so interruption cannot destroy the last valid
 * ledger. Staging files from a crashed compaction are discarded on open.
 *
 * Compaction is opt-in and does not run on open. On success the ledger
 * handle continues from the compacted generation. Returns ATP_OK and
 * fills `report` (when non-NULL) on success.
 */
atp_status atp_ledger_compact(atp_ledger *ledger, atp_compact_report *report);

/*
 * Deterministic replay.
 *
 * Rebuild learned state by re-applying the ledger's committed observations
 * to a fresh graph in ledger id order (the order they were originally
 * observed in). Replay uses the same observe path as live sync, so the
 * rebuilt state is what the original run produced from the same bytes.
 *
 * Outcome semantics:
 * - LEARNED   -> re-observed (training, episodic memory, familiarity) and
 *   mirrored into the snapshot; requires a retained payload.
 * - SKIPPED   -> mirrored only, never trained on.
 * - PENDING / FAILED -> excluded entirely: retryable reservations, not
 *   committed experience.
 * - WITHDRAWN -> excluded entirely: durably removed by an operator or by
 *   source deletion. The rebuilt state is what the entity would have been
 *   without them.
 *
 * A LEARNED entry without a retained payload (v1-migrated ledger) fails the
 * rebuild with ATP_ERR_FORMAT — the training input is gone, and a graph that
 * silently never saw those bytes would be a lie. An entry recorded under a
 * learning schema this core cannot replay (see atp_schema_can_replay) fails
 * with ATP_ERR_SCHEMA, naming the entry and its schema in the report.
 */

/**
 * Whether an observation recorded under learning schema `version` can be
 * replayed by this core. This is the single compatibility table: when a
 * learning-algorithm change bumps ATPERSON_SCHEMA_VERSION, decide its
 * compatibility class (see the schema version docs above) and record that
 * decision here, in the same change. Replay consults this predicate for
 * every entry; snapshots record their learning schema and load refuses
 * foreign ones.
 */
bool atp_schema_can_replay(uint32_t version);

/** Replay counters; zeroed on entry, filled on success. */
typedef struct atp_replay_report {
    /** LEARNED entries re-observed. */
    size_t replayed;
    /** SKIPPED entries mirrored without training. */
    size_t mirrored;
    /** PENDING entries excluded. */
    size_t excluded_pending;
    /** FAILED entries excluded. */
    size_t excluded_failed;
    /** WITHDRAWN entries excluded. */
    size_t excluded_withdrawn;
    /** Ledger id of the entry that failed (0 when none did). */
    uint64_t failed_at_id;
    /** Schema version of the entry that failed (0 when none did, or when
     * the failure was not schema-related). */
    uint32_t failed_schema;
} atp_replay_report;

/**
 * Re-apply every committed observation in `ledger` to `graph` in id order.
 * `report` may be NULL. The graph is not cleared: callers pass a freshly
 * created graph for a rebuild.
 */
atp_status atp_replay_ledger(const atp_ledger *ledger, atp_graph *graph,
                             atp_replay_report *report);

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
