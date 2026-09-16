/*
 * Observation ledger.
 *
 * A durable, append-only record of every observation fed to the learning core.
 * Records are immutable once committed; an outcome change appends a small
 * patch record, so the log never rewrites a committed byte.
 *
 * Layout: `path` is the record log, `<path>.off` is the durable commit marker.
 * The record wire format (entry/patch bodies, little-endian framing, CRC)
 * lives in ledger_format.c; the commit marker and crash ordering of the
 * record-then-marker write live in ledger_off.c; torn-tail truncation,
 * marker healing and the v1 -> v2 migration live in ledger_recover.c; the
 * in-memory dedup index lives in ledger_index.c. See those files for the
 * detailed contracts. Shared constants and the atp_ledger layout are in
 * ledger_internal.h.
 *
 * This file owns the public lifecycle: open (header creation, v1 detection,
 * recovery), append, outcome patches, withdrawal, lookup/inspection and
 * compaction, plus the single-release path every error exit funnels
 * through.
 */

#include "ledger_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *atp_ledger_strdup(const char *value) {
    const size_t length = strlen(value);
    char *copy = malloc(length + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length + 1u);
    return copy;
}

static char *atp_ledger_strdup_suffix(const char *value, const char *suffix) {
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    char *copy = malloc(value_len + suffix_len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, value_len);
    memcpy(copy + value_len, suffix, suffix_len + 1u);
    return copy;
}

/* Write a framed record at the end of the log, fsync it, then publish the
 * commit marker. On failure the record bytes may be present but the marker
 * still fences the previous prefix, so recovery discards them. */
static atp_status atp_ledger_commit_record(atp_ledger *ledger, const unsigned char *record,
                                           size_t record_len) {
    if (fseek(ledger->log, 0, SEEK_END) != 0) {
        return ATP_ERR_IO;
    }
    if (fwrite(record, 1u, record_len, ledger->log) != record_len) {
        return ATP_ERR_IO;
    }
    if (!atp_fsync(ledger->log)) {
        return ATP_ERR_IO;
    }
    ledger->committed_offset += record_len;
    if (!atp_write_off(ledger)) {
        return ATP_ERR_IO;
    }
    return ATP_OK;
}

static void atp_ledger_release(atp_ledger *ledger) {
    if (!ledger) {
        return;
    }
    if (ledger->log) {
        fclose(ledger->log);
    }
    if (ledger->payloads) {
        for (size_t i = 0u; i < ledger->count; ++i) {
            free(ledger->payloads[i]);
        }
    }
    free(ledger->log_path);
    free(ledger->log_tmp_path);
    free(ledger->off_path);
    free(ledger->off_tmp_path);
    free(ledger->entries);
    free(ledger->payloads);
    free(ledger->payload_lens);
    free(ledger->index);
    free(ledger);
}

atp_ledger *atp_ledger_open(const char *path, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!path || path[0] == '\0') {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    atp_ledger *ledger = calloc(1u, sizeof(*ledger));
    if (!ledger) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }
    ledger->log_path = atp_ledger_strdup(path);
    ledger->log_tmp_path = atp_ledger_strdup_suffix(path, ".tmp");
    ledger->off_path = atp_ledger_strdup_suffix(path, ".off");
    ledger->off_tmp_path = atp_ledger_strdup_suffix(path, ".off.tmp");
    if (!ledger->log_path || !ledger->log_tmp_path || !ledger->off_path || !ledger->off_tmp_path) {
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }

    /* Discard staging files left behind by a crashed rename. */
    remove(ledger->log_tmp_path);
    remove(ledger->off_tmp_path);

    FILE *log = fopen(ledger->log_path, "rb+");
    if (!log) {
        /* Create the record log header atomically so a crash during creation
         * never leaves a partially written header in place. */
        FILE *tmp = fopen(ledger->log_tmp_path, "wb");
        if (!tmp) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
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
        bool ok = fwrite(header, 1u, sizeof(header), tmp) == sizeof(header) && atp_fsync(tmp) &&
                  fclose(tmp) == 0;
        if (!ok) {
            remove(ledger->log_tmp_path);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        if (rename(ledger->log_tmp_path, ledger->log_path) != 0) {
            remove(ledger->log_tmp_path);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        log = fopen(ledger->log_path, "rb+");
        if (!log) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
    }

    unsigned char header[ATP_LEDGER_HEADER_SIZE];
    if (fread(header, 1u, sizeof(header), log) != sizeof(header)) {
        fclose(log);
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    const bool is_v1 = header[0] == ATP_LEDGER_FILE_MAGIC_0 &&
                       header[1] == ATP_LEDGER_FILE_MAGIC_1 &&
                       header[2] == ATP_LEDGER_FILE_MAGIC_2 &&
                       header[3] == ATP_LEDGER_FILE_MAGIC_3 &&
                       header[4] == ATP_LEDGER_FILE_MAGIC_4 &&
                       header[5] == ATP_LEDGER_FILE_MAGIC_5 &&
                       header[6] == ATP_LEDGER_FILE_MAGIC_6 &&
                       header[7] == ATP_LEDGER_V1_FILE_MAGIC_7 &&
                       atp_load_u32_le(&header[8]) == ATP_LEDGER_V1_VERSION;
    if (is_v1) {
        /* v1 log: migrate to v2 before recovery. The migration validates the
         * v1 committed prefix, transforms each record to v2 shape
         * (payload_len 0), streams to a temp file, fsyncs, and renames —
         * a crash leaves either the intact v1 log or the complete v2 log.
         * The old marker is discarded; recovery rewrites it from the
         * migrated prefix. */
        fclose(log);
        const atp_status migrated = atp_ledger_migrate_v1(ledger);
        if (migrated != ATP_OK) {
            atp_ledger_release(ledger);
            if (status) {
                *status = migrated;
            }
            return NULL;
        }
        log = fopen(ledger->log_path, "rb+");
        if (!log) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        if (fread(header, 1u, sizeof(header), log) != sizeof(header)) {
            fclose(log);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
    }

    if (header[0] != ATP_LEDGER_FILE_MAGIC_0 || header[1] != ATP_LEDGER_FILE_MAGIC_1 ||
        header[2] != ATP_LEDGER_FILE_MAGIC_2 || header[3] != ATP_LEDGER_FILE_MAGIC_3 ||
        header[4] != ATP_LEDGER_FILE_MAGIC_4 || header[5] != ATP_LEDGER_FILE_MAGIC_5 ||
        header[6] != ATP_LEDGER_FILE_MAGIC_6 || header[7] != ATP_LEDGER_FILE_MAGIC_7 ||
        atp_load_u32_le(&header[8]) != ATPERSON_LEDGER_VERSION) {
        fclose(log);
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    ledger->log = log;
    const atp_status recovered = atp_ledger_recover(ledger);
    if (recovered != ATP_OK) {
        atp_ledger_release(ledger);
        if (status) {
            *status = recovered;
        }
        return NULL;
    }
    return ledger;
}

void atp_ledger_destroy(atp_ledger *ledger) {
    if (!ledger) {
        return;
    }
    if (ledger->log) {
        /* Flush durability here; atp_ledger_release owns the single fclose. */
        atp_fsync(ledger->log);
    }
    atp_ledger_release(ledger);
}

atp_ledger_result atp_ledger_append(atp_ledger *ledger, const char *source_id,
                                    const char *author_did, uint64_t observed_at,
                                    uint64_t content_digest, uint32_t schema_version,
                                    atp_ledger_outcome outcome, const void *payload,
                                    size_t payload_len, uint64_t *out_id, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (out_id) {
        *out_id = 0u;
    }
    if (!ledger || !source_id || !atp_ledger_outcome_valid((uint8_t)outcome)) {
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
    if (payload_len > ATPERSON_LEDGER_PAYLOAD_LIMIT ||
        (!payload && payload_len > 0u)) {
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

    /* Pre-allocate before the durable write so a memory failure never leaves
     * the log ahead of the in-memory index. */
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

    unsigned char *body = malloc(ATP_LEDGER_PAYLOAD_MAX + payload_len);
    if (!body) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t body_len =
        atp_serialize_entry(body, &entry, (const unsigned char *)payload, payload_len);
    unsigned char *record = malloc(9u + body_len);
    if (!record) {
        free(body);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return ATP_LEDGER_NOT_FOUND;
    }
    const size_t record_len =
        atp_build_record(record, ATP_LEDGER_RECORD_ENTRY, body, body_len);

    /* Increment the count before the durable commit so the offset marker
     * records the post-append count; recover() trusts that fence. */
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
            /* The entry is durably committed; the in-memory payload mirror
             * is gone but the log retains it. Mark the slot payload-less in
             * memory only — a restart re-reads it from the log. */
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

    if (out_id) {
        *out_id = entry.id;
    }
    return ATP_LEDGER_NEW;
}

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
    for (size_t i = 0u; i < (size_t)ledger->count; ++i) {
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
    for (size_t i = 0u; i < (size_t)ledger->count; ++i) {
        atp_ledger_entry *entry = &ledger->entries[i];
        if (entry->outcome != ATP_LEDGER_OUTCOME_WITHDRAWN &&
            strcmp(entry->author_did, author_did) == 0 &&
            atp_ledger_set_outcome(ledger, entry->id, ATP_LEDGER_OUTCOME_WITHDRAWN) == ATP_OK) {
            withdrawn++;
        }
    }
    return withdrawn;
}

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
        /* No retained payload: v1-migrated entry or an empty observation.
         * Honest absence, not an error. */
        return ATP_OK;
    }
    /* Re-verify against the entry's content digest: corruption or a
     * mismatching payload is a format error, never returned as data. */
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

/*
 * Compaction.
 *
 * Streams the logical ledger — one entry record per entry, patches
 * flattened to final outcomes, WITHDRAWN payloads dropped — to the staging
 * file, then swaps it in with the same crash ordering as every other
 * ledger write: fsync the staging file, remove the commit marker, rename.
 *
 * A crash before the marker removal leaves the original log authoritative
 * (recovery heals the marker from it). A crash between the removal and
 * the rename is healed on the next open: no marker means self-heal mode,
 * which validates the longest prefix of whichever log file is present —
 * the original pre-rename, the compacted log post-rename. Neither state
 * destroys the last valid ledger.
 */
atp_status atp_ledger_compact(atp_ledger *ledger, atp_compact_report *report) {
    if (report) {
        memset(report, 0, sizeof(*report));
    }
    if (!ledger) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    /* Count patch records in the committed prefix: the history compaction
     * folds away. One bounded scan, record headers only. */
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
                /* The committed prefix is validated by recovery; a bad
                 * length here means the handle is inconsistent. */
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

    /* One entry record per entry, in id order. The in-memory state already
     * holds the flattened outcomes (recovery and set_outcome apply patches
     * on load), so the compacted log needs no patch records at all. */
    for (size_t i = 0u; i < ledger->count && result == ATP_OK; ++i) {
        const atp_ledger_entry *entry = &ledger->entries[i];
        const unsigned char *payload = ledger->payloads[i];
        size_t payload_len = ledger->payload_lens[i];

        if (entry->outcome == ATP_LEDGER_OUTCOME_WITHDRAWN && payload) {
            /* Withdrawn bytes are unreachable by design: replay excludes
             * the entry, dedup still suppresses the key, withdrawal is
             * durable. Keep the tombstone, drop the bytes — in the
             * compacted log and in the live handle, so the handle
             * matches what a reopen of the compacted generation sees. */
            payload = NULL;
            payload_len = 0u;
            free(ledger->payloads[i]);
            ledger->payloads[i] = NULL;
            ledger->payload_lens[i] = 0u;
            if (report) {
                report->payloads_dropped++;
            }
        }

        const size_t body_len = atp_serialize_entry(body, entry, payload, payload_len);
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
        /* The staging file is complete and durable. Swap ordering: remove
         * the marker first, then rename. A crash anywhere in this window
         * is healed by self-heal recovery from whichever log is present. */
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

    /* Reopen the compacted log and heal the marker from it. The handle
     * continues from the compacted generation. */
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
