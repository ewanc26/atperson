/*
 * Ledger read-only inspection.
 *
 * Owns dedup lookup, logical entry enumeration, count reporting, and
 * integrity-checked payload/context access. These operations never mutate
 * durable ledger state.
 */

#include "internal.h"

#include <stdint.h>
#include <string.h>

atp_ledger_result atp_ledger_lookup(const atp_ledger *ledger, const char *source_id,
                                    uint64_t content_digest, atp_ledger_entry *out_entry) {
    if (out_entry) {
        memset(out_entry, 0, sizeof(*out_entry));
    }
    if (!ledger || !source_id) {
        return ATP_LEDGER_NOT_FOUND;
    }
    const int64_t found = atp_ledger_index_find(ledger, source_id, content_digest);
    if (found < 0) {
        return ATP_LEDGER_NOT_FOUND;
    }
    const atp_ledger_entry *entry = &ledger->entries[(size_t)found];
    if (out_entry) {
        *out_entry = *entry;
    }
    return atp_ledger_is_committed(entry->outcome) ? ATP_LEDGER_EXISTS_COMMITTED
                                                   : ATP_LEDGER_EXISTS_PENDING;
}

uint64_t atp_ledger_count(const atp_ledger *ledger) {
    return ledger ? (uint64_t)ledger->count : 0u;
}

atp_status atp_ledger_entry_at(const atp_ledger *ledger, size_t index,
                               atp_ledger_entry *out_entry) {
    if (!ledger || !out_entry) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= ledger->count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_entry = ledger->entries[index];
    return ATP_OK;
}

atp_status atp_ledger_entry_payload(const atp_ledger *ledger, uint64_t id, void *out,
                                    size_t capacity, size_t *out_len) {
    if (out_len) {
        *out_len = 0u;
    }
    if (!ledger || !out_len) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (id == 0u || id > (uint64_t)ledger->count) {
        return ATP_ERR_NOT_FOUND;
    }
    const size_t index = (size_t)(id - 1u);
    const unsigned char *payload = ledger->payloads[index];
    const size_t payload_len = ledger->payload_lens[index];
    if (!payload || payload_len == 0u) {
        return ATP_OK;
    }
    if (atp_ledger_digest(payload, payload_len) != ledger->entries[index].content_digest) {
        return ATP_ERR_FORMAT;
    }
    if (out) {
        if (capacity < payload_len) {
            return ATP_ERR_INVALID_ARGUMENT;
        }
        memcpy(out, payload, payload_len);
    }
    *out_len = payload_len;
    return ATP_OK;
}

atp_status atp_ledger_entry_context(const atp_ledger *ledger, uint64_t id,
                                    atp_conversation_context *out) {
    if (!ledger || !out) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (id == 0u || id > (uint64_t)ledger->count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out = ledger->contexts[(size_t)(id - 1u)];
    return ATP_OK;
}
