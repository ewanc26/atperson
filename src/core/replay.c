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
 * Mirrored entries retain the conversational context recorded with them
 * (issue #49), so reply/quote continuity survives a replay rebuild and not
 * just a snapshot restore. Context is metadata: it is never trained on, and
 * an entry recorded before context capture replays with empty context.
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
        /* Schema 1: the byte-oriented learning algorithm. Replay is an
         * adapter migration: the legacy tokenizer (atp_tokenize with
         * schema_version 1) reproduces schema-1 token identity exactly,
         * so atp_graph_observe_with_memory replays these entries
         * byte-for-byte. */
        return true;
    case 2u:
        /* Schema 2: the Unicode tokenization contract (issue #8) —
         * NFKC_Casefold normalization, category-based token boundaries,
         * UTF-8 sanitization. Current learning algorithm. */
        return true;
    default:
        return false;
    }
}

static bool atp_replay_architecture_equal(
    const atp_neural_architecture *left,
    const atp_neural_architecture *right) {
    return left->version == right->version &&
           left->embedding_dim == right->embedding_dim &&
           left->input_dim == right->input_dim &&
           left->hidden_layer_count == right->hidden_layer_count &&
           left->output_dim == right->output_dim &&
           memcmp(left->hidden_widths, right->hidden_widths,
                  sizeof(left->hidden_widths)) == 0;
}

static atp_status atp_validate_replay_migrations(
    const atp_ledger *ledger, const atp_graph *graph,
    const atp_neural_migration *migrations, size_t migration_count) {
    if (migration_count == 0u) {
        return ATP_OK;
    }
    if (!migrations ||
        !atp_replay_architecture_equal(&graph->neural_architecture,
                                       &migrations[0].source)) {
        return ATP_ERR_MIGRATION;
    }

    uint64_t previous_boundary = 0u;
    for (size_t i = 0u; i < migration_count; ++i) {
        if (atp_neural_migration_validate(&migrations[i]) != ATP_OK ||
            (i != 0u &&
             migrations[i].ledger_boundary_id < previous_boundary) ||
            (i != 0u &&
             !atp_replay_architecture_equal(&migrations[i - 1u].target,
                                            &migrations[i].source))) {
            return ATP_ERR_MIGRATION;
        }
        previous_boundary = migrations[i].ledger_boundary_id;
    }

    const size_t ledger_count = atp_ledger_count(ledger);
    if (ledger_count == 0u) {
        return previous_boundary == 0u ? ATP_OK : ATP_ERR_MIGRATION;
    }
    atp_ledger_entry last;
    if (atp_ledger_entry_at(ledger, ledger_count - 1u, &last) != ATP_OK) {
        return ATP_ERR_FORMAT;
    }
    return previous_boundary <= last.id ? ATP_OK : ATP_ERR_MIGRATION;
}

static atp_status atp_apply_replay_migrations(
    atp_graph *graph, const atp_neural_migration *migrations,
    size_t migration_count, size_t *migration_index, uint64_t boundary,
    atp_replay_report *report) {
    while (*migration_index < migration_count &&
           migrations[*migration_index].ledger_boundary_id == boundary) {
        const atp_status expanded =
            atp_graph_expand_neural(graph, &migrations[*migration_index]);
        if (expanded != ATP_OK) {
            if (report) {
                report->failed_at_id = boundary;
            }
            return expanded;
        }
        (*migration_index)++;
        if (report) {
            report->migrations_applied++;
        }
    }
    return ATP_OK;
}

atp_status atp_replay_ledger_with_migrations(
    const atp_ledger *ledger, atp_graph *graph,
    const atp_neural_migration *migrations, size_t migration_count,
    atp_replay_report *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!ledger || !graph || (migration_count != 0u && !migrations)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_status status =
        atp_validate_replay_migrations(ledger, graph, migrations,
                                       migration_count);
    if (status != ATP_OK) {
        return status;
    }

    size_t migration_index = 0u;
    status = atp_apply_replay_migrations(
        graph, migrations, migration_count, &migration_index, 0u, report);
    if (status != ATP_OK) {
        return status;
    }

    const size_t count = atp_ledger_count(ledger);
    unsigned char *payload = NULL;
    size_t payload_capacity = 0u;
    for (size_t i = 0u; i < count; ++i) {
        atp_ledger_entry entry;
        if (atp_ledger_entry_at(ledger, i, &entry) != ATP_OK) {
            if (report) {
                report->failed_at_id = (uint64_t)i + 1u;
            }
            status = ATP_ERR_FORMAT;
            break;
        }

        if (migration_index < migration_count &&
            migrations[migration_index].ledger_boundary_id < entry.id) {
            if (report) {
                report->failed_at_id =
                    migrations[migration_index].ledger_boundary_id;
            }
            status = ATP_ERR_MIGRATION;
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

        atp_conversation_context context;
        if (atp_ledger_entry_context(ledger, entry.id, &context) != ATP_OK) {
            if (report) {
                report->failed_at_id = entry.id;
            }
            status = ATP_ERR_FORMAT;
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
            const atp_status mirrored =
                atp_graph_add_ledger_entry_with_context(graph, &entry, &context);
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
            const atp_status mirrored =
                atp_graph_add_ledger_entry_with_context(graph, &entry, &context);
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

        status = atp_apply_replay_migrations(
            graph, migrations, migration_count, &migration_index, entry.id,
            report);
        if (status != ATP_OK) {
            break;
        }
    }

    if (status == ATP_OK && migration_index != migration_count) {
        if (report) {
            report->failed_at_id =
                migrations[migration_index].ledger_boundary_id;
        }
        status = ATP_ERR_MIGRATION;
    }

    free(payload);
    return status;
}

atp_status atp_replay_ledger(const atp_ledger *ledger, atp_graph *graph,
                             atp_replay_report *report) {
    return atp_replay_ledger_with_migrations(ledger, graph, NULL, 0u, report);
}
