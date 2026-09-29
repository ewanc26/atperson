/*
 * Ledger compaction.
 *
 * Streams the logical ledger to a staging file, flattens outcome patches,
 * drops withdrawn payload bytes, fsyncs, swaps generations, and republishes
 * the commit marker without changing logical entry ordering or ids.
 * Retained conversational context (issue #49) is metadata, not payload, so it
 * survives compaction for every entry, withdrawn included.
 */

#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atp_status atp_ledger_compact_release(atp_ledger *ledger, atp_compact_report *report,
                                             uint64_t release_bytes) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!ledger) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    uint64_t patches = 0u;
    if (report) {
        if (fseek(ledger->log, (long)ATP_LEDGER_HEADER_SIZE, SEEK_SET) != 0) {
            return ATP_ERR_IO;
        }
        uint64_t position = ATP_LEDGER_HEADER_SIZE;
        while (position + 9u <= ledger->committed_offset) {
            unsigned char head[9u];
            if (!atp_read_file(ledger->log, head, sizeof(head))) {
                return ATP_ERR_IO;
            }
            const uint32_t payload_len = atp_load_u32_le(&head[0]);
            if (payload_len == 0u || payload_len > ATP_LEDGER_BODY_MAX ||
                position + 9u + (size_t)payload_len > ledger->committed_offset) {
                return ATP_ERR_FORMAT;
            }
            if (head[8] == ATP_LEDGER_RECORD_PATCH) {
                patches++;
            }
            position += 9u + (size_t)payload_len;
            if (fseek(ledger->log, (long)position, SEEK_SET) != 0) {
                return ATP_ERR_IO;
            }
        }
        if (position != ledger->committed_offset) {
            return ATP_ERR_FORMAT;
        }
        report->patches_flattened = patches;
        report->bytes_before = ledger->committed_offset;
        if (fseek(ledger->log, 0, SEEK_END) != 0) {
            return ATP_ERR_IO;
        }
    }

    FILE *tmp = fopen(ledger->log_tmp_path, "wb");
    if (!tmp) {
        return ATP_ERR_IO;
    }

    unsigned char header[ATP_LEDGER_HEADER_SIZE];
    header[0] = ATP_LEDGER_FILE_MAGIC_0;
    header[1] = ATP_LEDGER_FILE_MAGIC_1;
    header[2] = ATP_LEDGER_FILE_MAGIC_2;
    header[3] = ATP_LEDGER_FILE_MAGIC_3;
    header[4] = ATP_LEDGER_FILE_MAGIC_4;
    header[5] = ATP_LEDGER_FILE_MAGIC_5;
    header[6] = ATP_LEDGER_FILE_MAGIC_6;
    header[7] = ATP_LEDGER_FILE_MAGIC_7;
    atp_store_u32_le(&header[8], ATPERSON_LEDGER_VERSION);
    if (fwrite(header, 1u, sizeof(header), tmp) != sizeof(header)) {
        fclose(tmp);
        remove(ledger->log_tmp_path);
        return ATP_ERR_IO;
    }

    atp_status result = ATP_OK;
    unsigned char *body = malloc(ATP_LEDGER_BODY_MAX);
    if (!body) {
        fclose(tmp);
        remove(ledger->log_tmp_path);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    unsigned char *record = malloc(ATP_LEDGER_RECORD_MAX);
    if (!record) {
        free(body);
        fclose(tmp);
        remove(ledger->log_tmp_path);
        return ATP_ERR_OUT_OF_MEMORY;
    }

    uint64_t released_bytes = 0u;
    for (size_t i = 0u; i < ledger->count && result == ATP_OK; ++i) {
        const atp_ledger_entry *entry = &ledger->entries[i];
        const unsigned char *payload = ledger->payloads[i];
        size_t payload_len = ledger->payload_lens[i];

        if (release_bytes > 0u && released_bytes < release_bytes && payload && payload_len > 0u &&
            payload_len != ATP_LEDGER_PAYLOAD_RELEASED &&
            (entry->outcome == ATP_LEDGER_OUTCOME_LEARNED ||
             entry->outcome == ATP_LEDGER_OUTCOME_SKIPPED)) {
            released_bytes += payload_len;
            payload = NULL;
            payload_len = ATP_LEDGER_PAYLOAD_RELEASED;
            free(ledger->payloads[i]);
            ledger->payloads[i] = NULL;
            ledger->payload_lens[i] = ATP_LEDGER_PAYLOAD_RELEASED;
            if (report) {
                report->payloads_released++;
            }
        }
        if (entry->outcome == ATP_LEDGER_OUTCOME_WITHDRAWN && payload) {
            payload = NULL;
            payload_len = 0u;
            free(ledger->payloads[i]);
            ledger->payloads[i] = NULL;
            ledger->payload_lens[i] = 0u;
            if (report) {
                report->payloads_dropped++;
            }
        }

        const size_t body_len =
            atp_serialize_entry(body, entry, payload, payload_len, &ledger->contexts[i]);
        const size_t record_len =
            atp_build_record(record, ATP_LEDGER_RECORD_ENTRY, body, body_len);
        if (fwrite(record, 1u, record_len, tmp) != record_len) {
            result = ATP_ERR_IO;
        } else if (report) {
            report->entries++;
        }
    }

    if (result == ATP_OK && (!atp_fsync(tmp) || fclose(tmp) != 0)) {
        result = ATP_ERR_IO;
    } else if (result == ATP_OK) {
        remove(ledger->off_path);
        if (rename(ledger->log_tmp_path, ledger->log_path) != 0) {
            result = ATP_ERR_IO;
        }
    }
    if (result != ATP_OK) {
        remove(ledger->log_tmp_path);
        free(body);
        free(record);
        return result;
    }

    if (fclose(ledger->log) != 0) {
        ledger->log = NULL;
        free(body);
        free(record);
        return ATP_ERR_IO;
    }
    ledger->log = NULL;
    FILE *log = fopen(ledger->log_path, "rb+");
    if (!log) {
        free(body);
        free(record);
        return ATP_ERR_IO;
    }
    ledger->log = log;
    if (fseek(ledger->log, 0, SEEK_END) != 0) {
        free(body);
        free(record);
        return ATP_ERR_IO;
    }
    const long end_position = ftell(ledger->log);
    if (end_position < 0) {
        free(body);
        free(record);
        return ATP_ERR_IO;
    }

    if (report) {
        report->bytes_after = (uint64_t)end_position;
    }
    ledger->committed_offset = (uint64_t)end_position;
    if (!atp_write_off(ledger)) {
        free(body);
        free(record);
        return ATP_ERR_IO;
    }
    free(body);
    free(record);
    return ATP_OK;
}

atp_status atp_ledger_compact(atp_ledger *ledger, atp_compact_report *report) {
    return atp_ledger_compact_release(ledger, report, 0u);
}

atp_status atp_ledger_release_payloads(atp_ledger *ledger, uint64_t max_bytes,
                                       atp_compact_report *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!ledger || max_bytes == 0u) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (ledger->committed_offset <= max_bytes) {
        if (report) {
            report->bytes_before = ledger->committed_offset;
            report->bytes_after = ledger->committed_offset;
        }
        return ATP_OK;
    }
    /* Nothing releasable (metadata alone exceeds the cap): do not rewrite the
     * whole log just to release zero payloads. */
    bool releasable = false;
    for (size_t i = 0u; i < ledger->count && !releasable; ++i) {
        releasable = ledger->payloads[i] && ledger->payload_lens[i] > 0u &&
                     ledger->payload_lens[i] != ATP_LEDGER_PAYLOAD_RELEASED &&
                     (ledger->entries[i].outcome == ATP_LEDGER_OUTCOME_LEARNED ||
                      ledger->entries[i].outcome == ATP_LEDGER_OUTCOME_SKIPPED);
    }
    if (!releasable) {
        if (report) {
            report->bytes_before = ledger->committed_offset;
            report->bytes_after = ledger->committed_offset;
        }
        return ATP_OK;
    }
    /* Release down to 80% of the cap so the next append does not immediately
     * cross it again. */
    const uint64_t target = max_bytes - max_bytes / 5u;
    return atp_ledger_compact_release(ledger, report, ledger->committed_offset - target);
}
