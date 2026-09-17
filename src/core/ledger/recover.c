/*
 * Observation ledger crash recovery.
 *
 * Torn-tail truncation and marker healing: on open the committed prefix
 * (fenced by the .off marker when present, self-healed from the longest
 * valid prefix when not) is validated record-by-record. A torn tail beyond
 * the fence is truncated; a missing marker is rewritten from the recovered
 * prefix. A fenced prefix that fails validation is corruption, not a torn
 * tail, and is refused.
 *
 * Legacy-format migration (v1/v2 -> current) lives in migrate.c and runs
 * before recovery, so this module only ever sees current-format records.
 */

#include "internal.h"

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
            atp_conversation_context entry_context;
            if (!atp_parse_entry(payload, payload_size, &entry, &entry_payload,
                                 &entry_payload_len, &entry_context)) {
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
            ledger->contexts[ledger->count] = entry_context;
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
