/*
 * Observation ledger legacy-format migration (v1 -> v3, v2 -> v3).
 *
 * The durable log is append-only and format-versioned. A log written by an
 * older binary is migrated on open: its committed prefix (fenced by the
 * matching legacy marker when present, self-healed from the longest valid
 * prefix when not) is validated record-by-record, patch history is flattened
 * onto final entry outcomes, and the transformed current-version records are
 * streamed to a temp file, fsync'd, and renamed over the original. A crash at
 * any point leaves either the intact legacy log or the complete migrated log;
 * the stale marker is discarded so recovery rewrites it from the migrated
 * prefix.
 *
 * Migration is lossy only where the source format was already lossy: v1
 * entries retain no observation bytes, and neither v1 nor v2 entries carry
 * conversational context (issue #49). Migrated entries therefore report an
 * empty payload (v1) and empty context (both) rather than synthesising data.
 *
 * This module owns legacy parsing and the migration pass only. Current-format
 * recovery belongs to recover.c; record encoding belongs to format.c.
 */

#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Parse the entry fields shared by every legacy version: id, source id,
 * author DID, observed_at, content digest, schema version, outcome. Sets
 * *out_pos to the offset just past the outcome. Does not require the body to
 * end there: callers add their version-specific trailing fields.
 */
static bool atp_parse_entry_fields(const unsigned char *payload, size_t length,
                                   atp_ledger_entry *out, size_t *out_pos) {
    if (length < 38u) {
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

    *out_pos = pos;
    return true;
}

/* v1 entry body: the shared fields, ending at the outcome byte. */
static bool atp_parse_entry_v1(const unsigned char *payload, size_t length,
                               atp_ledger_entry *out) {
    size_t pos = 0u;
    return atp_parse_entry_fields(payload, length, out, &pos) && pos == length;
}

/* v2 entry body: the shared fields plus a trailing length-prefixed payload.
 * `out_payload` aliases `payload` and is NULL when the payload is empty. */
static bool atp_parse_entry_v2(const unsigned char *payload, size_t length,
                               atp_ledger_entry *out, const unsigned char **out_payload,
                               size_t *out_payload_len) {
    size_t pos = 0u;
    if (!atp_parse_entry_fields(payload, length, out, &pos) || pos + 4u > length) {
        return false;
    }
    const uint32_t payload_len = atp_load_u32_le(payload + pos);
    pos += 4u;
    if (payload_len > ATPERSON_LEDGER_PAYLOAD_LIMIT || pos + (size_t)payload_len != length) {
        return false;
    }
    *out_payload = payload_len > 0u ? payload + pos : NULL;
    *out_payload_len = (size_t)payload_len;
    return true;
}

static bool atp_parse_legacy_entry(unsigned version, const unsigned char *payload, size_t length,
                                   atp_ledger_entry *out, const unsigned char **out_payload,
                                   size_t *out_payload_len) {
    *out_payload = NULL;
    *out_payload_len = 0u;
    if (version == ATP_LEDGER_V1_VERSION) {
        return atp_parse_entry_v1(payload, length, out);
    }
    return atp_parse_entry_v2(payload, length, out, out_payload, out_payload_len);
}

static void atp_release_legacy_entries(atp_ledger_entry *entries, unsigned char **payloads,
                                       size_t count) {
    if (payloads) {
        for (size_t i = 0u; i < count; ++i) {
            free(payloads[i]);
        }
    }
    free(entries);
    free(payloads);
}

static bool atp_grow_legacy_entries(atp_ledger_entry **entries, unsigned char ***payloads,
                                    size_t **payload_lens, size_t *capacity, size_t needed) {
    if (needed <= *capacity) {
        return true;
    }
    size_t next = *capacity ? *capacity : 16u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u) {
            return false;
        }
        next *= 2u;
    }
    atp_ledger_entry *grown = realloc(*entries, next * sizeof(*grown));
    if (!grown) {
        return false;
    }
    *entries = grown;
    unsigned char **grown_payloads = realloc(*payloads, next * sizeof(*grown_payloads));
    if (!grown_payloads) {
        return false;
    }
    *payloads = grown_payloads;
    size_t *grown_lens = realloc(*payload_lens, next * sizeof(*grown_lens));
    if (!grown_lens) {
        return false;
    }
    *payload_lens = grown_lens;
    *capacity = next;
    return true;
}

atp_status atp_ledger_migrate(atp_ledger *ledger, unsigned source_version) {
    const bool is_v1 = source_version == ATP_LEDGER_V1_VERSION;
    const unsigned char off_magic_7 =
        is_v1 ? ATP_LEDGER_V1_OFF_MAGIC_7 : ATP_LEDGER_V2_OFF_MAGIC_7;
    const size_t body_limit =
        is_v1 ? ATP_LEDGER_PAYLOAD_MAX : ATP_LEDGER_PAYLOAD_MAX + ATPERSON_LEDGER_PAYLOAD_LIMIT;

    FILE *log = fopen(ledger->log_path, "rb");
    if (!log) {
        return ATP_ERR_IO;
    }

    /* The legacy marker fences the committed prefix; without it the longest
     * valid prefix is used, matching current-format recovery. */
    uint64_t fence = 0u;
    bool have_off = false;
    FILE *off = fopen(ledger->off_path, "rb");
    if (off) {
        unsigned char off_header[ATP_LEDGER_OFF_HEADER_SIZE];
        const bool ok =
            fread(off_header, 1u, sizeof(off_header), off) == sizeof(off_header) &&
            off_header[0] == ATP_LEDGER_OFF_MAGIC_0 &&
            off_header[1] == ATP_LEDGER_OFF_MAGIC_1 &&
            off_header[2] == ATP_LEDGER_OFF_MAGIC_2 &&
            off_header[3] == ATP_LEDGER_OFF_MAGIC_3 &&
            off_header[4] == ATP_LEDGER_OFF_MAGIC_4 &&
            off_header[5] == ATP_LEDGER_OFF_MAGIC_5 &&
            off_header[6] == ATP_LEDGER_OFF_MAGIC_6 && off_header[7] == off_magic_7 &&
            atp_load_u32_le(&off_header[8]) == source_version;
        fclose(off);
        if (ok) {
            fence = atp_load_u64_le(&off_header[20]);
            have_off = true;
        }
    }

    if (fseek(log, 0, SEEK_END) != 0) {
        fclose(log);
        return ATP_ERR_IO;
    }
    const long end_position = ftell(log);
    if (end_position < 0) {
        fclose(log);
        return ATP_ERR_IO;
    }
    const uint64_t file_size = (uint64_t)end_position;
    if (!have_off) {
        fence = file_size;
    } else if (fence < ATP_LEDGER_HEADER_SIZE || fence > file_size) {
        /* The marker points beyond the log: bytes were lost after the marker
         * was made durable. Refuse rather than migrate a partial history. */
        fclose(log);
        return ATP_ERR_FORMAT;
    }
    if (fseek(log, (long)ATP_LEDGER_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(log);
        return ATP_ERR_IO;
    }

    FILE *tmp = fopen(ledger->log_tmp_path, "wb");
    if (!tmp) {
        fclose(log);
        return ATP_ERR_IO;
    }
    unsigned char target_header[ATP_LEDGER_HEADER_SIZE];
    target_header[0] = ATP_LEDGER_FILE_MAGIC_0;
    target_header[1] = ATP_LEDGER_FILE_MAGIC_1;
    target_header[2] = ATP_LEDGER_FILE_MAGIC_2;
    target_header[3] = ATP_LEDGER_FILE_MAGIC_3;
    target_header[4] = ATP_LEDGER_FILE_MAGIC_4;
    target_header[5] = ATP_LEDGER_FILE_MAGIC_5;
    target_header[6] = ATP_LEDGER_FILE_MAGIC_6;
    target_header[7] = ATP_LEDGER_FILE_MAGIC_7;
    atp_store_u32_le(&target_header[8], ATPERSON_LEDGER_VERSION);
    if (fwrite(target_header, 1u, sizeof(target_header), tmp) != sizeof(target_header)) {
        fclose(tmp);
        fclose(log);
        remove(ledger->log_tmp_path);
        return ATP_ERR_IO;
    }

    unsigned char *buffer = malloc(ATP_LEDGER_RECORD_MAX);
    unsigned char *body = malloc(ATP_LEDGER_BODY_MAX);
    if (!buffer || !body) {
        free(buffer);
        free(body);
        fclose(tmp);
        fclose(log);
        remove(ledger->log_tmp_path);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    atp_ledger_entry *entries = NULL;
    unsigned char **payloads = NULL;
    size_t *payload_lens = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    atp_status result = ATP_OK;

    uint64_t position = ATP_LEDGER_HEADER_SIZE;
    bool torn = false;
    while (position < fence) {
        const size_t remaining = (size_t)(fence - position);
        if (remaining < 9u || !atp_read_file(log, &buffer[0], 4u) ||
            !atp_read_file(log, &buffer[4], 4u)) {
            torn = true;
            break;
        }
        const uint32_t payload_len = atp_load_u32_le(&buffer[0]);
        const uint32_t crc = atp_load_u32_le(&buffer[4]);
        if (payload_len == 0u || payload_len > body_limit ||
            9u + (size_t)payload_len > remaining || !atp_read_file(log, &buffer[8], 1u) ||
            !atp_read_file(log, &buffer[9], payload_len) ||
            atp_ledger_checksum(&buffer[9], payload_len) != crc) {
            torn = true;
            break;
        }

        const uint8_t type = buffer[8];
        if (type == ATP_LEDGER_RECORD_ENTRY) {
            atp_ledger_entry entry;
            const unsigned char *entry_payload = NULL;
            size_t entry_payload_len = 0u;
            if (!atp_parse_legacy_entry(source_version, &buffer[9], payload_len, &entry,
                                        &entry_payload, &entry_payload_len) ||
                entry.id != (uint64_t)count + 1u) {
                torn = true;
                break;
            }
            if (!atp_grow_legacy_entries(&entries, &payloads, &payload_lens, &capacity,
                                         count + 1u)) {
                result = ATP_ERR_OUT_OF_MEMORY;
                break;
            }
            if (entry_payload_len > 0u) {
                unsigned char *owned = malloc(entry_payload_len);
                if (!owned) {
                    result = ATP_ERR_OUT_OF_MEMORY;
                    break;
                }
                memcpy(owned, entry_payload, entry_payload_len);
                payloads[count] = owned;
            } else {
                payloads[count] = NULL;
            }
            payload_lens[count] = entry_payload_len;
            entries[count] = entry;
            count++;
        } else if (type == ATP_LEDGER_RECORD_PATCH) {
            uint64_t patch_id = 0u;
            uint8_t patch_outcome = 0u;
            if (!atp_parse_patch(&buffer[9], payload_len, &patch_id, &patch_outcome) ||
                patch_id == 0u || patch_id > (uint64_t)count ||
                !atp_ledger_outcome_valid(patch_outcome)) {
                torn = true;
                break;
            }
            atp_ledger_entry *entry = &entries[patch_id - 1u];
            if (atp_ledger_is_committed(entry->outcome) &&
                (atp_ledger_outcome)patch_outcome == ATP_LEDGER_OUTCOME_PENDING) {
                torn = true;
                break;
            }
            entry->outcome = (atp_ledger_outcome)patch_outcome;
        } else {
            torn = true;
            break;
        }
        position += 9u + (size_t)payload_len;
    }

    if (result == ATP_OK) {
        if (have_off && torn) {
            /* A fenced prefix that fails validation is corruption, not a torn
             * tail: refuse rather than migrate a partial history. */
            result = ATP_ERR_FORMAT;
        } else {
            /* Stream the flattened entries as current-version records with
             * empty conversational context. */
            for (size_t i = 0u; i < count && result == ATP_OK; ++i) {
                const size_t body_len = atp_serialize_entry(body, &entries[i], payloads[i],
                                                             payload_lens[i], NULL);
                const size_t record_len =
                    atp_build_record(buffer, ATP_LEDGER_RECORD_ENTRY, body, body_len);
                if (fwrite(buffer, 1u, record_len, tmp) != record_len) {
                    result = ATP_ERR_IO;
                }
            }
            if (result == ATP_OK) {
                if (!atp_fsync(tmp) || fclose(tmp) != 0) {
                    result = ATP_ERR_IO;
                }
                tmp = NULL;
            }
            if (result == ATP_OK) {
                /* The temp file is complete and durable: swap it in, then drop
                 * the stale legacy marker so recovery rewrites it from the
                 * migrated log. */
                if (rename(ledger->log_tmp_path, ledger->log_path) != 0) {
                    result = ATP_ERR_IO;
                } else {
                    remove(ledger->off_path);
                }
            }
        }
    }

    if (result != ATP_OK) {
        if (tmp) {
            fclose(tmp);
        }
        remove(ledger->log_tmp_path);
    }
    free(buffer);
    free(body);
    atp_release_legacy_entries(entries, payloads, count);
    free(payload_lens);
    fclose(log);
    return result;
}
