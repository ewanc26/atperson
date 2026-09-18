#ifndef ATPERSON_CORE_H
#define ATPERSON_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATPERSON_EMBEDDING_DIM 16u
#define ATPERSON_NEURAL_MAX_HIDDEN_LAYERS 4u
#define ATPERSON_NEURAL_ARCHITECTURE_VERSION 1u
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
 *
 * Version 6 persists the explicit neural architecture descriptor
 * (ATP_SECTION_ARCH) and variable-length network/node payloads, so a graph
 * trained at any supported topology round-trips exactly, including its
 * plasticity-importance values. v6 files are identified by the "ATPERSN6"
 * magic; v5 files keep the "ATPERSN5" magic and load through the legacy
 * compatibility path at the named legacy architecture. v5 snapshots stay
 * byte-for-byte compatible and are still written by legacy graphs until an
 * explicit migration changes their topology.
 */
#define ATPERSON_SNAPSHOT_VERSION 6u
/* Previous portable fragment format, still loadable as the legacy graph. */
#define ATPERSON_SNAPSHOT_VERSION_V5 5u

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
 *
 * v3 appends the observation's conversational context (issue #24/#49) to each
 * entry record: the reply root/parent and quote target URIs, each a
 * length-prefixed string that is empty when absent. Context is planning/audit
 * metadata, never learnable bytes, so it does not affect the content digest or
 * learning. v1 and v2 logs are migrated on open; migrated entries carry empty
 * context, the honest value for records that predate context capture.
 */
#define ATPERSON_LEDGER_VERSION 3u

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
 * The schema version describes the observation-input contract only. The
 * neural topology (issue #65/#72) is separate model-generation state: it is
 * persisted in snapshot v6 and reproduced by replay from the caller-supplied
 * architecture. Running the same ledger through a different topology is a
 * deliberate new generation, not a schema change.
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

/*
 * Bounded provenance log for valence events (issue #13). The log explains
 * recent updates; the folded per-token state and counters are the full
 * accounting (they never evict). Evicted entries are counted, never faked.
 */
#define ATPERSON_VALENCE_EVENT_CAPACITY 1024u

/*
 * Concurrency contract.
 *
 * The C23 core is single-threaded by design. No core object (atp_graph,
 * atp_ledger) contains internal locking; every function assumes it owns
 * its arguments exclusively for the duration of the call. Callers own
 * serialisation: either confine each object to one thread, or serialise
 * every access through an explicit lock in the runtime layer.
 *
 * Const-qualified read paths are safe to call concurrently with each
 * other only while no writer is active — the core does not use
 * atomics, so concurrent read+write on one object is undefined
 * behaviour even for "read-only" calls (recall mutates recall
 * counters; lookup consults derived indexes).
 *
 * The C++23 runtime parallelises around this contract, not through
 * it: ingestion, I/O and orchestration may run on worker threads, but
 * every touch of a core object passes through one owner thread or an
 * explicit serialisation point. Learning stays deterministic —
 * parallel work must not change what an observation learns, ledger
 * commit order, or replay results.
 */

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
    ATP_ERR_SCHEMA = 6,
    /** A configured resource ceiling (node or edge capacity max) was
     * reached. The observation is rejected whole — never partially
     * learned. Distinct from ATP_ERR_OUT_OF_MEMORY: the allocator is
     * fine, the budget is the limit. */
    ATP_ERR_CAPACITY = 7
} atp_status;

typedef struct atp_graph atp_graph;
typedef struct atp_ledger atp_ledger;

/*
 * Durable neural topology descriptor (issue #65).
 *
 * Version 1 describes the current legacy scorer exactly: 16-dimensional token
 * embeddings, their pairwise concatenation as a 32-wide input, one 16-unit
 * tanh hidden layer, and one sigmoid output. The descriptor is explicit now so
 * later variable-shape snapshots and deterministic capacity migrations have a
 * stable C ABI instead of inferring topology from compile-time constants.
 *
 * The current core accepts only this legacy shape; exposing the descriptor
 * does not yet make topology configurable and does not change learning
 * semantics or snapshot bytes.
 */
typedef struct atp_neural_architecture {
    uint32_t version;
    uint32_t embedding_dim;
    uint32_t input_dim;
    uint32_t hidden_layer_count;
    uint32_t hidden_widths[ATPERSON_NEURAL_MAX_HIDDEN_LAYERS];
    uint32_t output_dim;
} atp_neural_architecture;

/*
 * Inspectable storage accounting for the active architecture. Parameter bytes
 * count trainable values once; shared_learned_state_bytes also includes the
 * matching plasticity-importance values. Per-node learned bytes include both
 * the embedding and its importance vector.
 */
typedef struct atp_neural_architecture_report {
    atp_neural_architecture architecture;
    uint64_t shared_parameter_count;
    uint64_t shared_parameter_bytes;
    uint64_t shared_learned_state_bytes;
    uint64_t per_node_embedding_bytes;
    uint64_t per_node_learned_state_bytes;
} atp_neural_architecture_report;

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
    /*
     * Learning rate for experience-derived valence (issue #13), in (0, 1].
     * Each explicit valence event moves a token's score by
     * `valence += rate * (signal - valence)`, so the score is an exponential
     * moving average of event signals, bounded in [-1, 1] by construction and
     * reversible by contrary evidence. 0 selects the default (0.25).
     */
    float valence_rate;
    /* Episode capacity (0 selects ATPERSON_EPISODE_DEFAULT_CAPACITY). */
    size_t episode_capacity;
    /*
     * Resource ceilings for the semantic graph (issue #9). 0 = unlimited.
     * These are a resource budget, not a retention policy: reaching a
     * ceiling rejects the whole observation with ATP_ERR_CAPACITY rather
     * than silently dropping vocabulary. Pruning with provenance is a
     * separate, future decision.
     */
    size_t node_capacity_max;
    size_t edge_capacity_max;
    /*
     * Plasticity control (issue #59). Mitigates catastrophic forgetting in
     * online scorer parameter and embedding updates. Off by default (false/0.0).
     */
    bool enable_plasticity_control;
    float plasticity_threshold;
    float plasticity_scale;
} atp_graph_config;

/** Statistics for online scorer plasticity control (issue #59). */
typedef struct atp_plasticity_report {
    uint64_t steps_total;
    uint64_t steps_protected;
    uint64_t parameters_protected;
} atp_plasticity_report;

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
    /* Observations rejected whole by a resource ceiling (issue #9). */
    uint64_t capacity_rejections;
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

/*
 * Conversational context attached to one mirrored ledger entry (issue #24).
 * Planning/audit metadata, never learned input: the reply root/parent and
 * quote target URIs identify what an observation responded to without
 * flattening thread structure into text. Empty strings mean absent — a
 * top-level post, a non-quote, or a parent deleted before the fetch.
 * Context persists in optional snapshot section 10; pre-#24 snapshots load
 * with empty context.
 */
#define ATPERSON_CONTEXT_URI_BYTES 256u

typedef struct atp_conversation_context {
    char reply_root_uri[ATPERSON_CONTEXT_URI_BYTES];
    char reply_parent_uri[ATPERSON_CONTEXT_URI_BYTES];
    char quote_uri[ATPERSON_CONTEXT_URI_BYTES];
} atp_conversation_context;

/**
 * Return a stable default configuration. The graph starts with no vocabulary,
 * no edges, and randomly initialised neural parameters derived from `seed`.
 */
atp_graph_config atp_graph_default_config(void);

/** Create an empty language graph at the named legacy topology. */
atp_graph *atp_graph_create(const atp_graph_config *config);

/**
 * Create an empty language graph at an explicit neural architecture
 * (issue #65/#72). The descriptor must satisfy the strict validation rules:
 * non-zero embedding/output dimensions with `input_dim == embedding_dim * 2`,
 * 1..ATPERSON_NEURAL_MAX_HIDDEN_LAYERS hidden layers with non-zero active
 * widths, scalar output, and bounded parameter/workspace arithmetic. Returns
 * NULL (failing closed, never partially) for any invalid descriptor.
 *
 * The topology becomes authoritative learned-state metadata: this graph
 * persists it in snapshot v6 and cannot be written by the v5 writer. Replay
 * of a ledger into a graph created here is deterministic for a fixed
 * architecture, seed and schema.
 */
atp_graph *atp_graph_create_with_architecture(const atp_graph_config *config,
                                              const atp_neural_architecture *architecture);

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

/** Return the named legacy architecture implemented by current snapshots. */
atp_neural_architecture atp_neural_legacy_architecture(void);

/** Read the active graph's explicit neural topology descriptor (issue #65). */
atp_status atp_graph_neural_architecture(const atp_graph *graph,
                                         atp_neural_architecture *out_architecture);

/** Read topology plus parameter/storage accounting for inspection. */
atp_status atp_graph_neural_report(const atp_graph *graph,
                                   atp_neural_architecture_report *out_report);

/** Read plasticity control statistics (issue #59). */
atp_status atp_graph_plasticity_report(const atp_graph *graph, atp_plasticity_report *out_report);

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

/**
 * Update the resource ceilings on a live graph (issue #9). 0 = unlimited.
 *
 * Ceilings are deployment policy, not graph data: atp_graph_load restores
 * the graph with unlimited ceilings regardless of what the saving process
 * had configured. Apply the budget after loading with this call. Lowering
 * a ceiling below the current count does not evict anything — existing
 * nodes and edges stay, further growth is rejected.
 */
void atp_graph_set_capacity(atp_graph *graph, size_t node_capacity_max,
                            size_t edge_capacity_max);

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
 * As atp_ledger_append, but also retains `context` (issue #24/#49) inline in
 * the entry record so a replay rebuild restores it. `context` may be NULL:
 * the entry then carries empty context, identical to atp_ledger_append. Any
 * URI at or beyond ATPERSON_CONTEXT_URI_BYTES is rejected with
 * ATP_ERR_INVALID_ARGUMENT before any mutation.
 */
atp_ledger_result atp_ledger_append_with_context(
    atp_ledger *ledger, const char *source_id, const char *author_did, uint64_t observed_at,
    uint64_t content_digest, uint32_t schema_version, atp_ledger_outcome outcome,
    const void *payload, size_t payload_len, const atp_conversation_context *context,
    uint64_t *out_id, atp_status *status);

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

/**
 * Copy the conversational context retained with entry `id` into `out`
 * (issue #24/#49). Absent identifiers are empty strings. Entries written by
 * atp_ledger_append, or migrated from a pre-v3 log, yield an all-empty
 * context, the honest value for records that never carried one.
 *
 * Returns ATP_ERR_NOT_FOUND for an unknown id and ATP_ERR_INVALID_ARGUMENT
 * for NULL arguments. Read-only: never mutates ledger or learned state.
 */
atp_status atp_ledger_entry_context(const atp_ledger *ledger, uint64_t id,
                                    atp_conversation_context *out);

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
 * The neural topology is model-generation state, orthogonal to the
 * observation schema: the caller supplies the graph (and therefore the
 * architecture), replay never consults host hardware, and a rebuild at the
 * persisted v6 architecture reproduces that generation deterministically.
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

/** Record one ledger entry plus its conversational context (issue #24).
 * `context` may be NULL: the entry then mirrors with empty context. */
atp_status atp_graph_add_ledger_entry_with_context(
    atp_graph *graph, const atp_ledger_entry *entry, const atp_conversation_context *context);

/** Copy the conversational context of the i-th mirrored entry (0-based). */
atp_status atp_graph_ledger_context(const atp_graph *graph, size_t index,
                                    atp_conversation_context *out_context);

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

/** Distinct summary tokens tracked per episode group before the union is
 * treated as saturated (always scanned, keeping the prefilter
 * conservative). */
#define ATPERSON_GROUP_TOKEN_MAX 32u

/**
 * One derived episode group (issue #55). Episodes whose summary-token sets
 * are identical share a group. `key` is the FNV-1a hash of the summary node
 * indices; `tokens` is the union of member summary tokens, used to prefilter
 * recall. All fields are derived from persisted episode fields -- a rebuilt
 * graph reproduces identical groups.
 */
typedef struct atp_episode_group {
    uint64_t key;
    uint32_t tokens[ATPERSON_GROUP_TOKEN_MAX];
    uint32_t token_count;
    uint64_t evictions;
} atp_episode_group;

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
 * Evidence-gated recall policy. The default (`atp_recall_default_config`)
 * reproduces the historical eager behaviour byte-identically: no minimum
 * overlap, no episode prefilter bound, recall enabled.
 *
 * `min_overlap` is the minimum exact-overlap score an episode must reach to
 * be returned. `max_prefilter` bounds how many of the most recent episodes
 * are scanned; 0 means unbounded. `disable` skips recall entirely.
 */
typedef struct atp_recall_config {
    float min_overlap;
    size_t max_prefilter;
    bool disable;
} atp_recall_config;

atp_recall_config atp_recall_default_config(void);

/** Why recall returned what it did. */
typedef enum atp_recall_gate {
    ATP_RECALL_GATE_NONE = 0,
    ATP_RECALL_GATE_DISABLED,
    ATP_RECALL_GATE_PREFILTER,
    ATP_RECALL_GATE_MIN_OVERLAP,
} atp_recall_gate;

/** Evidence for one recall call: how many episodes existed, were scanned,
 * matched with nonzero overlap, and survived the gate. `groups_total` and
 * `groups_scanned` report the group-index prefilter (issue #55): how many
 * groups existed and how many survived the token-overlap prefilter. */
typedef struct atp_recall_report {
    size_t episodes_total;
    size_t episodes_scanned;
    size_t episodes_matched;
    size_t episodes_returned;
    atp_recall_gate gate;
    size_t groups_total;
    size_t groups_scanned;
} atp_recall_report;

/**
 * Recall the episodes whose summary tokens overlap the query, strongest
 * overlap first (ties broken by recency, then ledger id). Recalled episodes
 * get their `recall_count` incremented and `last_recall_at` set to `at_epoch`;
 * query tokens are matched without mutating the vocabulary.
 *
 * `config` is optional (NULL = default eager behaviour). `report`, when
 * non-NULL, receives the scan/gate evidence for this call.
 */
atp_status atp_graph_recall(atp_graph *graph, const char *query, uint64_t at_epoch,
                            const atp_recall_config *config, atp_recall_report *report,
                            atp_episode *out, size_t capacity, size_t *out_count);

/** Number of episodes currently in memory. */
size_t atp_graph_episode_count(const atp_graph *graph);

/**
 * Derived episode groups (issue #55). `out` receives up to `capacity`
 * groups; `*out_count` receives the total group count (which may exceed
 * capacity). Groups are ordered by first member in episode insertion
 * order. Membership is derived from persisted episode fields alone.
 */
atp_status atp_graph_episode_groups(const atp_graph *graph, atp_episode_group *out,
                                    size_t capacity, size_t *out_count);

/**
 * Copy the ledger ids of the episodes in group `group_id`, in insertion
 * order, into `out` (up to `capacity`); `*out_count` receives the member
 * count. `ATP_ERR_NOT_FOUND` when the group id is out of range.
 */
atp_status atp_graph_episode_group_members(const atp_graph *graph, uint32_t group_id,
                                           uint64_t *out, size_t capacity, size_t *out_count);

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

/**
 * True when `token` is in the graph's vocabulary (it has been observed at
 * least once). Read-only; never interns. Runtime callers use this to honour
 * the valence contract — valence attaches to experienced subjects only —
 * without attempting an event and interpreting ATP_ERR_NOT_FOUND.
 */
bool atp_graph_has_token(const atp_graph *graph, const char *token);

/*
 * Experience-derived valence (issue #13).
 *
 * Valence is a slowly learned per-token score in [-1, 1] updated ONLY from
 * explicit experience events — never from exposure, content, or any
 * developer-authored seed. The empty-start invariant holds: a fresh graph
 * has no valence records at all, and observation alone never creates one.
 * A token that has been observed but never valued reads neutral (0.0).
 *
 * Evidence contract — what counts as an event:
 * - ACTION: the outcome of an action the entity itself took (e.g. a post
 *   that was well received scores positive, one that was rejected or
 *   erroring scores negative);
 * - INTERACTION: a direct interaction signal from another actor (reply,
 *   like, mention, block — the caller maps the signal to [-1, 1]);
 * - APPROACH: the entity chose to engage with the subject;
 * - AVOID: the entity chose not to engage with the subject.
 *
 * The caller decides what real-world event maps to each kind and signal
 * value; the core records it, folds it into the score, and retains
 * provenance. Nothing is inferred: a signal of 0 records an event but
 * moves the score toward 0 (neutral evidence).
 *
 * Update equation: valence' = valence + rate * (signal - valence), rate =
 * config.valence_rate in (0, 1]. Bounded in [-1, 1] by construction
 * (an EMA of values in [-1, 1] stays in [-1, 1]). No time-based decay:
 * valence changes only when events arrive, so the score is exactly the
 * folded evidence. Reversal: contrary evidence pulls the EMA across zero;
 * the event counters make the competition inspectable.
 */
typedef enum atp_valence_kind {
    ATP_VALENCE_ACTION = 1,
    ATP_VALENCE_INTERACTION = 2,
    ATP_VALENCE_APPROACH = 3,
    ATP_VALENCE_AVOID = 4
} atp_valence_kind;

/** Inspectable valence state for one token. */
typedef struct atp_valence_state {
    char token[ATPERSON_TOKEN_BYTES];
    float valence; /* in [-1, 1] */
    uint64_t event_count;
    uint64_t positive_events; /* signals > 0 */
    uint64_t negative_events; /* signals < 0 */
    uint64_t last_event_at;
} atp_valence_state;

/** One bounded-log provenance entry, in arrival order. */
typedef struct atp_valence_event {
    atp_valence_kind kind;
    float signal; /* in [-1, 1] */
    uint64_t at_epoch;
    char token[ATPERSON_TOKEN_BYTES];
    char source_id[ATPERSON_LEDGER_SOURCE_BYTES];
} atp_valence_event;

/**
 * Record one explicit valence event for `token`. The token must already be
 * known to the vocabulary (observed at least once): valence attaches to
 * experienced subjects, and interning vocabulary from a valence event would
 * let a single event create learned state, so unknown tokens return
 * ATP_ERR_NOT_FOUND and mutate nothing. `signal` is clamped to [-1, 1];
 * `source_id` is provenance (an AT URI or action id) and is never
 * interpreted. Events append to the bounded provenance log (oldest evicted
 * when full; the eviction counter tracks loss).
 */
atp_status atp_graph_valence_event(atp_graph *graph, const char *token, atp_valence_kind kind,
                                  float signal, uint64_t at_epoch, const char *source_id);

/**
 * Read the valence state for `token`. ATP_ERR_NOT_FOUND when the token is
 * unknown or has never received an event. Read-only.
 */
atp_status atp_graph_valence(const atp_graph *graph, const char *token, atp_valence_state *out);

/**
 * Copy the i-th valence record (0-based, in vocabulary node-index order —
 * the order tokens were first observed in). Read-only.
 */
atp_status atp_graph_valence_at(const atp_graph *graph, size_t index, atp_valence_state *out);

/** Number of tokens with valence records. */
size_t atp_graph_valence_count(const atp_graph *graph);

/**
 * Copy up to `capacity` provenance events, newest first. `out_count` is
 * always set when non-null. Read-only.
 */
atp_status atp_graph_valence_log(const atp_graph *graph, atp_valence_event *out, size_t capacity,
                                 size_t *out_count);

/** Total provenance entries evicted from the bounded log. */
uint64_t atp_graph_valence_log_evictions(const atp_graph *graph);

#ifdef __cplusplus
}
#endif

#endif
