/*
 * Ledger outcome mutation.
 *
 * Outcome changes are append-only patch records. Withdrawal helpers build
 * on the same primitive so no committed log bytes are rewritten in place.
 */

#include "ledger_internal.h"

#include <stdint.h>
#include <string.h>

atp_status atp_ledger_set_outcome(atp_ledger *ledger, uint64_t id, atp_ledger_outcome outcome) {
    if (!ledger) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (id == 0u || id > (uint64_t)ledger->count) {
        return ATP_ERR_NOT_FOUND;
    }
    if (!atp_ledger_outcome_valid((uint8_t)outcome)) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    atp_ledger_entry *entry = &ledger->entries[id - 1u];
    if (atp_ledger_is_committed(entry->outcome) && outcome == ATP_LEDGER_OUTCOME_PENDING) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (entry->outcome == outcome) {
        return ATP_OK;
    }

    unsigned char payload[9u];
    atp_store_u64_le(payload, id);
    payload[8] = (unsigned char)outcome;
    unsigned char record[ATP_LEDGER_RECORD_MAX];
    const size_t record_len =
        atp_build_record(record, ATP_LEDGER_RECORD_PATCH, payload, sizeof(payload));
    const atp_status committed = atp_ledger_commit_record(ledger, record, record_len);
    if (committed != ATP_OK) {
        return committed;
    }

    entry->outcome = outcome;
    return ATP_OK;
}

atp_status atp_ledger_withdraw(atp_ledger *ledger, uint64_t id) {
    return atp_ledger_set_outcome(ledger, id, ATP_LEDGER_OUTCOME_WITHDRAWN);
}

size_t atp_ledger_withdraw_source(atp_ledger *ledger, const char *source_id) {
    if (!ledger || !source_id) {
        return 0u;
    }
    size_t withdrawn = 0u;
    for (size_t i = 0u; i < ledger->count; ++i) {
        atp_ledger_entry *entry = &ledger->entries[i];
        if (entry->outcome != ATP_LEDGER_OUTCOME_WITHDRAWN &&
            strcmp(entry->source_id, source_id) == 0 &&
            atp_ledger_set_outcome(ledger, entry->id, ATP_LEDGER_OUTCOME_WITHDRAWN) == ATP_OK) {
            withdrawn++;
        }
    }
    return withdrawn;
}

size_t atp_ledger_withdraw_author(atp_ledger *ledger, const char *author_did) {
    if (!ledger || !author_did) {
        return 0u;
    }
    size_t withdrawn = 0u;
    for (size_t i = 0u; i < ledger->count; ++i) {
        atp_ledger_entry *entry = &ledger->entries[i];
        if (entry->outcome != ATP_LEDGER_OUTCOME_WITHDRAWN &&
            strcmp(entry->author_did, author_did) == 0 &&
            atp_ledger_set_outcome(ledger, entry->id, ATP_LEDGER_OUTCOME_WITHDRAWN) == ATP_OK) {
            withdrawn++;
        }
    }
    return withdrawn;
}
