/*
 * Observation ledger crash recovery and v1 migration.
 *
 * Torn-tail truncation and marker healing: on open the committed prefix
 * (fenced by the .off marker when present, self-healed from the longest
 * valid prefix when not) is validated record-by-record. A torn tail beyond
 * the fence is truncated; a missing marker is rewritten from the recovered
 * prefix. A fenced prefix that fails validation is corruption, not a torn
 * tail, and is refused.
 *
 * v1 -> v2 migration: the v1 committed prefix is validated record-by-record,
 * transformed to v2 shape (payload_len 0), streamed to a temporary file,
 * fsync'd, and renamed over the original. A crash at any point leaves either
 * the intact v1 log or the complete v2 log; the old marker is discarded and
 * rewritten from the migrated prefix. Patch history flattens to final entry
 * outcomes — the migration records the state, not the history, which is
 * documented behaviour.
 */

#include "ledger_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

atp_status atp_ledger_recover(atp_ledger *ledger) {
    uint64_t off_count = 0u;
    uint64_t off_offset = 0u;
    const bool have_off = atp_read_off(ledger, &off_count, &off_offset);
    bool self_heal = false;

    if (fseek(ledger->log, 0, SEEK_END) != 0) {
        return ATP_ERR_IO;
    }
    const long end_position = ftell(ledger->log);
    if (end_position < 0) {
        return ATP_ERR_IO;
    }
    const uint64_t file_size = (uint64_t)end_position;

    uint64_t fence;
    if (!have_off) {
        /* No durable marker. The log is the authority: use the longest valid
         * prefix and rewrite the marker afterwards (heal). */
        self_heal = true;
        fence = file_size;
    } else {
        if (off_offset < ATP_LEDGER_HEADER_SIZE || off_offset > file_size) {
            /* The marker points beyond the log: the log lost bytes after the
             * marker was made durable. Refuse to truncate silently. */
            return ATP_ERR_FORMAT;
        }
        fence = off_offset;
    }

    if (fseek(ledger->log, (long)ATP_LEDGER_HEADER_SIZE, SEEK_SET) != 0) {
        return ATP_ERR_IO;
    }

    uint64_t position = ATP_LEDGER_HEADER_SIZE;
    /* Entry records may carry a payload up to ATPERSON_LEDGER_PAYLOAD_LIMIT
     * on top of the entry fields, so records are read through a heap buffer
     * sized for the worst case. The 9-byte header is read first and the
     * length is bounds-checked before the rest is read, so parsing stays
     * bounded even on corrupted data. */
    unsigned char *buffer = malloc(ATP_LEDGER_RECORD_MAX);
    if (!buffer) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    while (position < fence) {
        const size_t remaining = (size_t)(fence - position);
        if (remaining < 9u) {
            /* Torn record header. */
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }
        if (!atp_read_file(ledger->log, &buffer[0], 4u) ||
            !atp_read_file(ledger->log, &buffer[4], 4u)) {
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }
        const uint32_t payload_len = atp_load_u32_le(&buffer[0]);
        const uint32_t crc = atp_load_u32_le(&buffer[4]);
        if (payload_len == 0u || payload_len > ATP_LEDGER_BODY_MAX ||
            9u + (size_t)payload_len > remaining) {
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }
        if (!atp_read_file(ledger->log, &buffer[8], 1u) ||
            !atp_read_file(ledger->log, &buffer[9], payload_len)) {
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }
        if (atp_ledger_checksum(&buffer[9], payload_len) != crc) {
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }

        const uint8_t type = buffer[8];
        const unsigned char *payload = &buffer[9];
        const size_t payload_size = (size_t)payload_len;

        if (type == ATP_LEDGER_RECORD_ENTRY) {
            atp_ledger_entry entry;
            const unsigned char *entry_payload = NULL;
            size_t entry_payload_len = 0u;
            if (!atp_parse_entry(payload, payload_size, &entry, &entry_payload,
                                 &entry_payload_len)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                free(buffer);
                return ATP_ERR_FORMAT;
            }
            if (entry.id != (uint64_t)ledger->count + 1u ||
                !atp_ledger_reserve_entries(ledger, ledger->count + 1u)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                free(buffer);
                return ATP_ERR_FORMAT;
            }
            if (entry_payload_len > 0u) {
                unsigned char *owned = malloc(entry_payload_len);
                if (!owned) {
                    free(buffer);
                    return ATP_ERR_OUT_OF_MEMORY;
                }
                memcpy(owned, entry_payload, entry_payload_len);
                ledger->payloads[ledger->count] = owned;
                ledger->payload_lens[ledger->count] = entry_payload_len;
            }
            ledger->entries[ledger->count] = entry;
            ledger->count++;
        } else if (type == ATP_LEDGER_RECORD_PATCH) {
            uint64_t patch_id = 0u;
            uint8_t patch_outcome = 0u;
            if (!atp_parse_patch(payload, payload_size, &patch_id, &patch_outcome)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                free(buffer);
                return ATP_ERR_FORMAT;
            }
            if (patch_id == 0u || patch_id > (uint64_t)ledger->count ||
                !atp_ledger_outcome_valid(patch_outcome)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                free(buffer);
                return ATP_ERR_FORMAT;
            }
            atp_ledger_entry *entry = &ledger->entries[patch_id - 1u];
            if (atp_ledger_is_committed(entry->outcome) &&
                (atp_ledger_outcome)patch_outcome == ATP_LEDGER_OUTCOME_PENDING) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                free(buffer);
                return ATP_ERR_FORMAT;
            }
            entry->outcome = (atp_ledger_outcome)patch_outcome;
        } else {
            if (self_heal) {
                fence = position;
                break;
            }
            free(buffer);
            return ATP_ERR_FORMAT;
        }
        position += 9u + (size_t)payload_len;
    }
    free(buffer);

    if (!self_heal && (uint64_t)ledger->count != off_count) {
        return ATP_ERR_FORMAT;
    }

    if (file_size > fence) {
        if (!atp_truncate_file(ledger->log, fence)) {
            return ATP_ERR_IO;
        }
    }
    ledger->committed_offset = fence;

    if (!have_off) {
        if (!atp_write_off(ledger)) {
            return ATP_ERR_IO;
        }
    }
    if (!atp_ledger_index_ensure_capacity(ledger)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }
    return ATP_OK;
}

/*
 * v1 -> v2 migration.
 *
 * The v1 entry body is the v2 entry body minus the trailing payload_len +
 * payload. Migration reads the v1 committed prefix (fenced by the v1 marker
 * when present, self-healed from the longest valid prefix when not),
 * validates every record, flattens patches onto their entries, and streams
 * the transformed v2 records to a temp file: fsync, rename, then discard the
 * old marker so recovery rewrites it from the migrated log.
 *
 * A crash before the rename leaves the intact v1 log; after it, the v2 log
 * is complete. v1 entries are migrated payload-less — their observation
 * bytes were never retained, and atp_ledger_entry_payload reports that
 * honestly instead of faking content.
 */
static bool atp_parse_entry_v1(const unsigned char *payload, size_t length,
                               atp_ledger_entry *out) {
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
    return pos == length;
}

atp_status atp_ledger_migrate_v1(atp_ledger *ledger) {
    FILE *log = fopen(ledger->log_path, "rb");
    if (!log) {
        return ATP_ERR_IO;
    }

    /* v1 marker fences the committed prefix; without it the longest valid
     * prefix is used (same self-heal policy as v2 recovery). */
    uint64_t fence = 0u;
    bool have_off = false;
    FILE *off = fopen(ledger->off_path, "rb");
    if (off) {
        unsigned char off_header[ATP_LEDGER_OFF_HEADER_SIZE];
        const bool ok = fread(off_header, 1u, sizeof(off_header), off) ==
                            sizeof(off_header) &&
                        off_header[0] == ATP_LEDGER_OFF_MAGIC_0 &&
                        off_header[1] == ATP_LEDGER_OFF_MAGIC_1 &&
                        off_header[2] == ATP_LEDGER_OFF_MAGIC_2 &&
                        off_header[3] == ATP_LEDGER_OFF_MAGIC_3 &&
                        off_header[4] == ATP_LEDGER_OFF_MAGIC_4 &&
                        off_header[5] == ATP_LEDGER_OFF_MAGIC_5 &&
                        off_header[6] == ATP_LEDGER_OFF_MAGIC_6 &&
                        off_header[7] == ATP_LEDGER_V1_OFF_MAGIC_7 &&
                        atp_load_u32_le(&off_header[8]) == ATP_LEDGER_V1_VERSION;
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
         * was made durable. Refuse, exactly like v2 recovery. */
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
    unsigned char v2_header[ATP_LEDGER_HEADER_SIZE];
    v2_header[0] = ATP_LEDGER_FILE_MAGIC_0;
    v2_header[1] = ATP_LEDGER_FILE_MAGIC_1;
    v2_header[2] = ATP_LEDGER_FILE_MAGIC_2;
    v2_header[3] = ATP_LEDGER_FILE_MAGIC_3;
    v2_header[4] = ATP_LEDGER_FILE_MAGIC_4;
    v2_header[5] = ATP_LEDGER_FILE_MAGIC_5;
    v2_header[6] = ATP_LEDGER_FILE_MAGIC_6;
    v2_header[7] = ATP_LEDGER_FILE_MAGIC_7;
    atp_store_u32_le(&v2_header[8], ATPERSON_LEDGER_VERSION);
    if (fwrite(v2_header, 1u, sizeof(v2_header), tmp) != sizeof(v2_header)) {
        fclose(tmp);
        fclose(log);
        remove(ledger->log_tmp_path);
        return ATP_ERR_IO;
    }

    /* Records are validated and transformed in one streaming pass. Entries
     * are held in memory so patches can be flattened onto them; the buffer
     * is bounded by the v1 record cap (v1 had no payloads). */
    unsigned char buffer[9u + ATP_LEDGER_PAYLOAD_MAX];
    atp_ledger_entry *entries = NULL;
    size_t count = 0u;
    size_t capacity = 0u;

    uint64_t position = ATP_LEDGER_HEADER_SIZE;
    bool torn = false;
    while (position < fence) {
        const size_t remaining = (size_t)(fence - position);
        if (remaining < 9u ||
            !atp_read_file(log, &buffer[0], 4u) ||
            !atp_read_file(log, &buffer[4], 4u)) {
            torn = true;
            break;
        }
        const uint32_t payload_len = atp_load_u32_le(&buffer[0]);
        const uint32_t crc = atp_load_u32_le(&buffer[4]);
        if (payload_len == 0u || payload_len > ATP_LEDGER_PAYLOAD_MAX ||
            9u + (size_t)payload_len > remaining ||
            !atp_read_file(log, &buffer[8], 1u) ||
            !atp_read_file(log, &buffer[9], payload_len) ||
            atp_ledger_checksum(&buffer[9], payload_len) != crc) {
            torn = true;
            break;
        }

        const uint8_t type = buffer[8];
        if (type == ATP_LEDGER_RECORD_ENTRY) {
            atp_ledger_entry entry;
            if (!atp_parse_entry_v1(&buffer[9], payload_len, &entry) ||
                entry.id != (uint64_t)count + 1u) {
                torn = true;
                break;
            }
            if (count == capacity) {
                const size_t next = capacity ? capacity * 2u : 16u;
                atp_ledger_entry *grown = realloc(entries, next * sizeof(*grown));
                if (!grown) {
                    free(entries);
                    fclose(tmp);
                    fclose(log);
                    remove(ledger->log_tmp_path);
                    return ATP_ERR_OUT_OF_MEMORY;
                }
                entries = grown;
                capacity = next;
            }
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

    atp_status result = ATP_OK;
    if (have_off && torn) {
        /* A fenced prefix that fails validation is corruption, not a torn
         * tail: refuse rather than migrate a partial history. */
        result = ATP_ERR_FORMAT;
    } else {
        /* Stream the flattened entries as v2 records (payload_len 0). */
        for (size_t i = 0u; i < count && result == ATP_OK; ++i) {
            unsigned char body[ATP_LEDGER_PAYLOAD_MAX];
            const size_t body_len = atp_serialize_entry(body, &entries[i], NULL, 0u);
            unsigned char record[ATP_LEDGER_RECORD_MAX];
            const size_t record_len = atp_build_record(record, ATP_LEDGER_RECORD_ENTRY, body,
                                                        body_len);
            if (fwrite(record, 1u, record_len, tmp) != record_len) {
                result = ATP_ERR_IO;
            }
        }
        if (result == ATP_OK && (!atp_fsync(tmp) || fclose(tmp) != 0)) {
            result = ATP_ERR_IO;
        } else if (result == ATP_OK) {
            /* The temp file is complete and durable: swap it in, then drop
             * the stale v1 marker so recovery rewrites it from the migrated
             * log. A crash before the rename keeps the v1 log intact; after
             * it, the v2 log is complete. */
            if (rename(ledger->log_tmp_path, ledger->log_path) != 0) {
                result = ATP_ERR_IO;
            } else {
                remove(ledger->off_path);
            }
        }
        if (result != ATP_OK) {
            remove(ledger->log_tmp_path);
        }
    }
    free(entries);
    fclose(log);
    return result;
}
