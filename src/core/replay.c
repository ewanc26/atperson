#include "atperson/core.h"

#include "internal.h"

#include <stdlib.h>
#include <string.h>

/*
 * Deterministic replay from the observation ledger.
 *
 * The ledger is the authority for what the entity has experienced; replay
 * reconstructs learned state from it alone. Entries are re-applied in ledger
 * id order — the order they were originally observed in — through the same
 * observe path sync uses, so a rebuilt graph is what the original run would
 * have produced from the same bytes.
 *
 * Outcome semantics are explicit:
 * - LEARNED: the retained payload is re-observed (training, episodic
 *   memory, familiarity) and the entry is mirrored into the snapshot.
 * - SKIPPED: the entry is mirrored only — observed but deliberately not
 *   learned from, exactly as the original run decided.
 * - PENDING / FAILED: excluded entirely. These are retryable reservations,
 *   not committed experience; a rebuild must not train on them.
 * - WITHDRAWN: excluded entirely. Durably removed by an operator or by
 *   source deletion; the rebuilt state is what the entity would have been
 *   without them.
 *
 * A LEARNED entry without a retained payload (a v1-migrated ledger) cannot
 * be replayed — the training input is gone — and fails the whole rebuild
 * rather than silently producing a graph that never saw those bytes. The
 * same applies to schema versions the core cannot replay: they fail with
 * ATP_ERR_SCHEMA instead of being reinterpreted.
 */

/*
 * Learning-schema compatibility table.
 *
 * Every entry replay consults atp_schema_can_replay. When a learning
 * change bumps ATPERSON_SCHEMA_VERSION, the same change records its
 * compatibility decision here:
 *
 * - Replay-compatible: add the old version below. Replay re-applies the
 *   entry through the current observe path, which reproduces the old
 *   learning effect.
 * - Adapter migration: add the old version below and dispatch on it in the
 *   replay loop to a versioned handler that reproduces the old behaviour.
 * - Incompatible: do not add it. Replay fails with ATP_ERR_SCHEMA naming
 *   the entry; the operator starts a fresh model generation.
 */
bool atp_schema_can_replay(uint32_t version) {
    switch (version) {
    case 1u:
        /* Schema 1: the current learning algorithm. */
        return true;
    default:
        return false;
    }
}

atp_status atp_replay_ledger(const atp_ledger *ledger, atp_graph *graph,
                             atp_replay_report *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!ledger || !graph) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const size_t count = atp_ledger_count(ledger);
    unsigned char *payload = NULL;
    size_t payload_capacity = 0u;
    atp_status status = ATP_OK;

    for (size_t i = 0u; i < count; ++i) {
        atp_ledger_entry entry;
        if (atp_ledger_entry_at(ledger, i, &entry) != ATP_OK) {
            if (report) {
                report->failed_at_id = (uint64_t)i + 1u;
            }
            status = ATP_ERR_FORMAT;
            break;
        }

        if (!atp_schema_can_replay(entry.schema_version)) {
            /* Entries recorded under a schema this core cannot replay are
             * intact data under the wrong algorithm: refuse with a
             * schema-specific status and name the entry, so the operator
             * knows exactly where the model-generation boundary is. */
            if (report) {
                report->failed_at_id = entry.id;
                report->failed_schema = entry.schema_version;
            }
            status = ATP_ERR_SCHEMA;
            break;
        }

        switch (entry.outcome) {
        case ATP_LEDGER_OUTCOME_LEARNED: {
            size_t payload_len = 0u;
            /* Length query first; the read re-verifies the content digest. */
            const atp_status probed =
                atp_ledger_entry_payload(ledger, entry.id, NULL, 0u, &payload_len);
            if (probed != ATP_OK || payload_len == 0u) {
                /* Payload-less LEARNED entry (v1-migrated ledger): the
                 * training input is gone. Fail the rebuild honestly. */
                if (report) {
                    report->failed_at_id = entry.id;
                }
                status = ATP_ERR_FORMAT;
                break;
            }
            if (payload_len + 1u > payload_capacity) {
                /* + 1: observe takes a NUL-terminated C string. */
                unsigned char *grown = realloc(payload, payload_len + 1u);
                if (!grown) {
                    if (report) {
                        report->failed_at_id = entry.id;
                    }
                    status = ATP_ERR_OUT_OF_MEMORY;
                    break;
                }
                payload = grown;
                payload_capacity = payload_len + 1u;
            }
            if (atp_ledger_entry_payload(ledger, entry.id, payload, payload_len,
                                         &payload_len) != ATP_OK) {
                if (report) {
                    report->failed_at_id = entry.id;
                }
                status = ATP_ERR_FORMAT;
                break;
            }
            payload[payload_len] = '\0';

            /* The payload is canonical text; observe takes a C string. The
             * digest was verified against these exact bytes on read. */
            const atp_status observed = atp_graph_observe_with_memory(
                graph, (const char *)payload, entry.source_id, entry.author_did,
                entry.observed_at, entry.content_digest, entry.schema_version, entry.id, NULL);
            if (observed != ATP_OK) {
                if (report) {
                    report->failed_at_id = entry.id;
                }
                status = observed;
                break;
            }
            const atp_status mirrored = atp_graph_add_ledger_entry(graph, &entry);
            if (mirrored != ATP_OK) {
                if (report) {
                    report->failed_at_id = entry.id;
                }
                status = mirrored;
                break;
            }
            if (report) {
                report->replayed++;
            }
            break;
        }
        case ATP_LEDGER_OUTCOME_SKIPPED: {
            /* Observed but deliberately not learned: mirror only, so the
             * rebuilt snapshot keeps skipped provenance inspectable. */
            const atp_status mirrored = atp_graph_add_ledger_entry(graph, &entry);
            if (mirrored != ATP_OK) {
                if (report) {
                    report->failed_at_id = entry.id;
                }
                status = mirrored;
                break;
            }
            if (report) {
                report->mirrored++;
            }
            break;
        }
        case ATP_LEDGER_OUTCOME_PENDING:
            /* Retryable reservation, not committed experience. */
            if (report) {
                report->excluded_pending++;
            }
            break;
        case ATP_LEDGER_OUTCOME_FAILED:
            /* Examined but untrainable; retryable, never learned. */
            if (report) {
                report->excluded_failed++;
            }
            break;
        case ATP_LEDGER_OUTCOME_WITHDRAWN:
            /* Durably excluded by an operator or source deletion: the
             * rebuilt state is what the entity would have been without
             * this observation. Never trained, never mirrored. */
            if (report) {
                report->excluded_withdrawn++;
            }
            break;
        default:
            if (report) {
                report->failed_at_id = entry.id;
            }
            status = ATP_ERR_FORMAT;
            break;
        }

        if (status != ATP_OK) {
            break;
        }
    }

    free(payload);
    return status;
}
