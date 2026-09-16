/*
 * Observation ledger in-memory dedup index.
 *
 * The unique index on (source id + digest) is rebuilt in memory on open and
 * maintained on append; the log is the authority for it, so the constraint
 * survives process restarts. The index is derived state: it is never
 * persisted, and recovery (ledger_recover.c) rebuilds it from the log.
 *
 * Open addressing, power-of-two capacity, slot entry == ATP_LEDGER_SLOT_EMPTY
 * means empty. Lookups confirm the candidate against the canonical entry
 * (digest + source id string), so a hash collision can never alias two
 * distinct observations.
 */

#include "ledger_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t atp_ledger_derive_key(const char *source, size_t source_len, uint64_t digest) {
    const unsigned char *cursor = (const unsigned char *)source;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < source_len; ++i) {
        hash ^= (uint64_t)cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    for (unsigned i = 0u; i < 8u; ++i) {
        hash ^= (uint64_t)(digest >> (8u * i)) & 0xffu;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool atp_ledger_reserve_entries(atp_ledger *ledger, size_t needed) {
    if (needed <= ledger->capacity) {
        return true;
    }
    size_t capacity = ledger->capacity ? ledger->capacity : 16u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    atp_ledger_entry *entries = realloc(ledger->entries, capacity * sizeof(*entries));
    if (!entries) {
        return false;
    }
    unsigned char **payloads = realloc(ledger->payloads, capacity * sizeof(*payloads));
    if (!payloads) {
        return false;
    }
    size_t *payload_lens = realloc(ledger->payload_lens, capacity * sizeof(*payload_lens));
    if (!payload_lens) {
        return false;
    }
    /* New slots start payload-less; append/recovery fill them in. */
    for (size_t i = ledger->capacity; i < capacity; ++i) {
        payloads[i] = NULL;
        payload_lens[i] = 0u;
    }
    ledger->entries = entries;
    ledger->payloads = payloads;
    ledger->payload_lens = payload_lens;
    ledger->capacity = capacity;
    return true;
}

bool atp_ledger_index_ensure_capacity(atp_ledger *ledger) {
    if (ledger->index && ledger->count + 1u <= ledger->index_size * 7u / 10u) {
        return true;
    }

    size_t target = ledger->index ? ledger->index_size * 2u : 8u;
    if (target < (ledger->count + 1u) * 10u / 7u + 1u) {
        while (target < (ledger->count + 1u) * 10u / 7u + 1u) {
            if (target > SIZE_MAX / 2u) {
                return false;
            }
            target *= 2u;
        }
    }

    atp_ledger_slot *slots = calloc(target, sizeof(*slots));
    if (!slots) {
        return false;
    }
    for (size_t i = 0u; i < target; ++i) {
        slots[i].entry = ATP_LEDGER_SLOT_EMPTY;
    }
    const size_t mask = target - 1u;
    for (size_t i = 0u; i < ledger->count; ++i) {
        const atp_ledger_entry *entry = &ledger->entries[i];
        const uint64_t key = atp_ledger_derive_key(entry->source_id, strlen(entry->source_id),
                                                   entry->content_digest);
        size_t slot = (size_t)(key & mask);
        while (slots[slot].entry != ATP_LEDGER_SLOT_EMPTY) {
            slot = (slot + 1u) & mask;
        }
        slots[slot] = (atp_ledger_slot){.key = key, .entry = (uint64_t)i};
    }

    free(ledger->index);
    ledger->index = slots;
    ledger->index_size = target;
    return true;
}

void atp_ledger_index_insert(atp_ledger *ledger, uint64_t key, size_t entry_index) {
    const size_t mask = ledger->index_size - 1u;
    size_t slot = (size_t)(key & mask);
    while (ledger->index[slot].entry != ATP_LEDGER_SLOT_EMPTY) {
        slot = (slot + 1u) & mask;
    }
    ledger->index[slot] = (atp_ledger_slot){.key = key, .entry = (uint64_t)entry_index};
}

int64_t atp_ledger_index_find(const atp_ledger *ledger, const char *source_id,
                              uint64_t digest) {
    if (ledger->count == 0u) {
        return -1;
    }
    const uint64_t key = atp_ledger_derive_key(source_id, strlen(source_id), digest);
    const size_t mask = ledger->index_size - 1u;
    size_t slot = (size_t)(key & mask);
    while (ledger->index[slot].entry != ATP_LEDGER_SLOT_EMPTY) {
        const atp_ledger_entry *entry = &ledger->entries[ledger->index[slot].entry];
        if (ledger->index[slot].key == key && entry->content_digest == digest &&
            strcmp(entry->source_id, source_id) == 0) {
            return (int64_t)ledger->index[slot].entry;
        }
        slot = (slot + 1u) & mask;
    }
    return -1;
}
