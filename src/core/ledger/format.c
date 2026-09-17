/*
 * Observation ledger record wire format.
 *
 * Owns the byte-level record contract shared by every ledger module:
 *
 *   entry rec  : len u32 LE | crc u32 LE | type=1 | id u64 LE
 *                | source_len u32 LE | source | author_len u32 LE | author
 *                | observed_at u64 LE | digest u64 LE
 *                | schema_version u32 LE | outcome u8
 *                | payload_len u32 LE | payload            (v2)
 *   patch rec  : len u32 LE | crc u32 LE | type=2 | id u64 LE | outcome u8
 *
 * v2 appends `payload_len` plus the canonical observation bytes to each entry
 * record, so a committed learnable entry can return the exact bytes
 * originally supplied to the learning core. `payload_len` 0 is the honest
 * shape for text-less observations and for entries migrated from v1, whose
 * bytes were never retained. The payload is covered by the record CRC and
 * re-verified against the entry's content digest on read.
 *
 * Multi-octet integers are little-endian, which is a deliberate format
 * decision (snapshot v1 remains host-oriented). A future format bump may
 * migrate either file independently.
 *
 * Helpers here are pure encode/decode: no file I/O, no ledger state beyond
 * the entry values they are handed.
 */

#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void atp_store_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8u) & 0xffu);
    out[2] = (unsigned char)((value >> 16u) & 0xffu);
    out[3] = (unsigned char)((value >> 24u) & 0xffu);
}

void atp_store_u64_le(unsigned char *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) {
        out[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

uint32_t atp_load_u32_le(const unsigned char *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8u) | ((uint32_t)in[2] << 16u) |
           ((uint32_t)in[3] << 24u);
}

uint64_t atp_load_u64_le(const unsigned char *in) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        value |= (uint64_t)in[i] << (8u * i);
    }
    return value;
}

/* FNV-1a checksum truncated to 32 bits. Deterministic and portable; this is
 * corruption/torn-tail detection, not a security primitive. */
uint32_t atp_ledger_checksum(const void *data, size_t length) {
    const unsigned char *cursor = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < length; ++i) {
        hash ^= (uint64_t)cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    return (uint32_t)(hash ^ (hash >> 32u));
}

uint64_t atp_ledger_digest(const void *data, size_t length) {
    const unsigned char *cursor = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < length; ++i) {
        hash ^= (uint64_t)cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool atp_ledger_outcome_valid(uint8_t value) {
    return value <= (uint8_t)ATP_LEDGER_OUTCOME_WITHDRAWN;
}

bool atp_ledger_is_committed(atp_ledger_outcome outcome) {
    return outcome == ATP_LEDGER_OUTCOME_LEARNED || outcome == ATP_LEDGER_OUTCOME_SKIPPED ||
           outcome == ATP_LEDGER_OUTCOME_WITHDRAWN;
}

size_t atp_serialize_entry(unsigned char *out, const atp_ledger_entry *entry,
                           const unsigned char *payload, size_t payload_len) {
    const size_t source_len = strlen(entry->source_id);
    const size_t author_len = strlen(entry->author_did);
    size_t pos = 0u;
    atp_store_u64_le(out + pos, entry->id);
    pos += 8u;
    atp_store_u32_le(out + pos, (uint32_t)source_len);
    pos += 4u;
    memcpy(out + pos, entry->source_id, source_len);
    pos += source_len;
    atp_store_u32_le(out + pos, (uint32_t)author_len);
    pos += 4u;
    memcpy(out + pos, entry->author_did, author_len);
    pos += author_len;
    atp_store_u64_le(out + pos, entry->observed_at);
    pos += 8u;
    atp_store_u64_le(out + pos, entry->content_digest);
    pos += 8u;
    atp_store_u32_le(out + pos, entry->schema_version);
    pos += 4u;
    out[pos++] = (unsigned char)entry->outcome;
    atp_store_u32_le(out + pos, (uint32_t)payload_len);
    pos += 4u;
    if (payload_len > 0u) {
        memcpy(out + pos, payload, payload_len);
        pos += payload_len;
    }
    return pos;
}

bool atp_parse_entry(const unsigned char *payload, size_t length, atp_ledger_entry *out,
                     const unsigned char **out_payload, size_t *out_payload_len) {
    if (length < 42u) {
        return false;
    }
    size_t pos = 0u;
    memset(out, 0, sizeof(*out));
    out->id = atp_load_u64_le(payload + pos);
    pos += 8u;

    const uint32_t source_len = atp_load_u32_le(payload + pos);
    pos += 4u;
    if (source_len == 0u || source_len >= ATPERSON_LEDGER_SOURCE_BYTES ||
        pos + (size_t)source_len > length) {
        return false;
    }
    memcpy(out->source_id, payload + pos, source_len);
    pos += source_len;
    out->source_id[source_len] = '\0';

    if (pos + 4u > length) {
        return false;
    }
    const uint32_t author_len = atp_load_u32_le(payload + pos);
    pos += 4u;
    if (author_len >= ATPERSON_LEDGER_AUTHOR_BYTES || pos + (size_t)author_len > length) {
        return false;
    }
    if (author_len > 0u) {
        memcpy(out->author_did, payload + pos, author_len);
        pos += author_len;
        out->author_did[author_len] = '\0';
    }

    if (pos + 8u > length) {
        return false;
    }
    out->observed_at = atp_load_u64_le(payload + pos);
    pos += 8u;
    if (pos + 8u > length) {
        return false;
    }
    out->content_digest = atp_load_u64_le(payload + pos);
    pos += 8u;
    if (pos + 4u > length) {
        return false;
    }
    out->schema_version = atp_load_u32_le(payload + pos);
    pos += 4u;
    if (pos + 1u > length) {
        return false;
    }
    const uint8_t outcome = payload[pos];
    if (!atp_ledger_outcome_valid(outcome)) {
        return false;
    }
    out->outcome = (atp_ledger_outcome)outcome;
    pos += 1u;

    if (pos + 4u > length) {
        return false;
    }
    const uint32_t payload_len = atp_load_u32_le(payload + pos);
    pos += 4u;
    if (payload_len > ATPERSON_LEDGER_PAYLOAD_LIMIT || pos + (size_t)payload_len != length) {
        return false;
    }
    *out_payload = payload + pos;
    *out_payload_len = payload_len;
    return true;
}

bool atp_parse_patch(const unsigned char *payload, size_t length, uint64_t *id,
                     uint8_t *outcome) {
    if (length != 9u) {
        return false;
    }
    *id = atp_load_u64_le(payload);
    *outcome = payload[8];
    return true;
}

size_t atp_build_record(unsigned char record[ATP_LEDGER_RECORD_MAX], uint8_t type,
                        const unsigned char *payload, size_t payload_len) {
    atp_store_u32_le(&record[0], (uint32_t)payload_len);
    atp_store_u32_le(&record[4], atp_ledger_checksum(payload, payload_len));
    record[8] = type;
    memcpy(&record[9], payload, payload_len);
    return 9u + payload_len;
}
