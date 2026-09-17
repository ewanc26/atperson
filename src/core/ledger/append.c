/*
 * Ledger append path.
 *
 * Validates a new observation, deduplicates it, preallocates all mutable
 * in-memory state before the durable write, commits the framed record, then
 * mirrors the committed entry/payload/context into memory and the dedup
 * index. atp_ledger_append is the context-less entry point; the
 * _with_context variant additionally retains conversational context inline in
 * the record so a replay rebuild restores it.
 */

#include "internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A context struct is valid when every URI is a NUL-terminated string shorter
 * than the fixed C-core capacity. NULL means "no context". */
static bool atp_context_valid(const atp_conversation_context *context) {
    if (!context) {
        return true;
    }
    return strnlen(context->reply_root_uri, ATPERSON_CONTEXT_URI_BYTES) <
               ATPERSON_CONTEXT_URI_BYTES &&
           strnlen(context->reply_parent_uri, ATPERSON_CONTEXT_URI_BYTES) <
               ATPERSON_CONTEXT_URI_BYTES &&
           strnlen(context->quote_uri, ATPERSON_CONTEXT_URI_BYTES) < ATPERSON_CONTEXT_URI_BYTES;
}

atp_ledger_result atp_ledger_append_with_context(
    atp_ledger *ledger, const char *source_id, const char *author_did, uint64_t observed_at,
    uint64_t content_digest, uint32_t schema_version, atp_ledger_outcome outcome,
    const void *payload, size_t payload_len, const atp_conversation_context *context,
    uint64_t *out_id, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (out_id) {
        *out_id = 0u;
    }
    if (!ledger || !source_id || !atp_ledger_outcome_valid((uint8_t)outcome) ||
        !atp_context_valid(context)) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t source_len = strlen(source_id);
    if (source_len == 0u || source_len >= ATPERSON_LEDGER_SOURCE_BYTES) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t author_len = author_did ? strlen(author_did) : 0u;
    if (author_len >= ATPERSON_LEDGER_AUTHOR_BYTES) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    if (payload_len > ATPERSON_LEDGER_PAYLOAD_LIMIT || (!payload && payload_len > 0u)) {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return ATP_LEDGER_NOT_FOUND;
    }

    atp_ledger_entry existing = {0};
    const atp_ledger_result found = atp_ledger_lookup(ledger, source_id, content_digest, &existing);
    if (found != ATP_LEDGER_NOT_FOUND) {
        if (out_id) {
            *out_id = existing.id;
        }
        return found;
    }

    if (!atp_ledger_reserve_entries(ledger, ledger->count + 1u) ||
        !atp_ledger_index_ensure_capacity(ledger)) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return ATP_LEDGER_NOT_FOUND;
    }

    atp_ledger_entry entry = {0};
    entry.id = (uint64_t)ledger->count + 1u;
    entry.observed_at = observed_at;
    entry.content_digest = content_digest;
    entry.schema_version = schema_version ? schema_version : ATPERSON_SCHEMA_VERSION;
    entry.outcome = outcome;
    memcpy(entry.source_id, source_id, source_len + 1u);
    memcpy(entry.author_did, author_did ? author_did : "", author_len + 1u);

    unsigned char *body = malloc(ATP_LEDGER_PAYLOAD_MAX + payload_len + ATP_LEDGER_CONTEXT_MAX);
    if (!body) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t body_len = atp_serialize_entry(body, &entry, (const unsigned char *)payload,
                                                payload_len, context);
    unsigned char *record = malloc(9u + body_len);
    if (!record) {
        free(body);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t record_len = atp_build_record(record, ATP_LEDGER_RECORD_ENTRY, body, body_len);

    ledger->count++;
    const atp_status committed = atp_ledger_commit_record(ledger, record, record_len);
    free(record);
    free(body);
    if (committed != ATP_OK) {
        ledger->count--;
        if (status) {
            *status = committed;
        }
        return ATP_LEDGER_NOT_FOUND;
    }

    ledger->entries[ledger->count - 1u] = entry;
    if (payload_len > 0u) {
        unsigned char *owned = malloc(payload_len);
        if (!owned) {
            ledger->payloads[ledger->count - 1u] = NULL;
            ledger->payload_lens[ledger->count - 1u] = 0u;
        } else {
            memcpy(owned, payload, payload_len);
            ledger->payloads[ledger->count - 1u] = owned;
            ledger->payload_lens[ledger->count - 1u] = payload_len;
        }
    }
    atp_ledger_index_insert(ledger, atp_ledger_derive_key(source_id, source_len, content_digest),
                            ledger->count - 1u);
    ledger->contexts[ledger->count - 1u] = context ? *context : (atp_conversation_context){0};

    if (out_id) {
        *out_id = entry.id;
    }
    return ATP_LEDGER_NEW;
}

atp_ledger_result atp_ledger_append(atp_ledger *ledger, const char *source_id,
                                    const char *author_did, uint64_t observed_at,
                                    uint64_t content_digest, uint32_t schema_version,
                                    atp_ledger_outcome outcome, const void *payload,
                                    size_t payload_len, uint64_t *out_id, atp_status *status) {
    return atp_ledger_append_with_context(ledger, source_id, author_did, observed_at,
                                          content_digest, schema_version, outcome, payload,
                                          payload_len, NULL, out_id, status);
}
