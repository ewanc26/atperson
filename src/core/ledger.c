/*
 * Observation ledger.
 *
 * A durable, append-only record of every observation fed to the learning core.
 * Records are immutable once committed; an outcome change appends a small
 * patch record, so the log never rewrites a committed byte.
 *
 * Layout: `path` is the record log, `<path>.off` is the durable commit marker.
 *
 *   log header : magic(8) | version u32 LE                     -> 12 bytes
 *   entry rec  : len u32 LE | crc u32 LE | type=1 | id u64 LE
 *                | source_len u32 LE | source | author_len u32 LE | author
 *                | observed_at u64 LE | digest u64 LE
 *                | schema_version u32 LE | outcome u8
 *   patch rec  : len u32 LE | crc u32 LE | type=2 | id u64 LE | outcome u8
 *   off header : magic(8) | version u32 LE | count u64 LE | offset u64 LE
 *                -> 28 bytes
 *
 * Multi-octet integers are little-endian, which is a deliberate format
 * decision (snapshot v1 remains host-oriented). A future format bump may
 * migrate either file independently.
 *
 * Correctness relies on write ordering: record bytes are written and fsync'd
 * to the log first, and only then is the commit marker staged in a temporary
 * file, fsync'd, and renamed into place. A crash at any point leaves the
 * previous committed prefix intact; recovery truncates a torn tail beyond the
 * committed offset and heals a missing marker from the valid log prefix.
 *
 * The unique index on (source id + digest) is rebuilt in memory on open and
 * maintained on append; the log is the authority for it, so the constraint
 * survives process restarts.
 */

#include "atperson/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#define ATP_LEDGER_FILE_MAGIC_0 'A'
#define ATP_LEDGER_FILE_MAGIC_1 'T'
#define ATP_LEDGER_FILE_MAGIC_2 'P'
#define ATP_LEDGER_FILE_MAGIC_3 'L'
#define ATP_LEDGER_FILE_MAGIC_4 'D'
#define ATP_LEDGER_FILE_MAGIC_5 'G'
#define ATP_LEDGER_FILE_MAGIC_6 '0'
#define ATP_LEDGER_FILE_MAGIC_7 '1'

#define ATP_LEDGER_OFF_MAGIC_0 'A'
#define ATP_LEDGER_OFF_MAGIC_1 'T'
#define ATP_LEDGER_OFF_MAGIC_2 'P'
#define ATP_LEDGER_OFF_MAGIC_3 'L'
#define ATP_LEDGER_OFF_MAGIC_4 'O'
#define ATP_LEDGER_OFF_MAGIC_5 'F'
#define ATP_LEDGER_OFF_MAGIC_6 '0'
#define ATP_LEDGER_OFF_MAGIC_7 '1'

#define ATP_LEDGER_RECORD_ENTRY 1u
#define ATP_LEDGER_RECORD_PATCH 2u

#define ATP_LEDGER_HEADER_SIZE 12u
#define ATP_LEDGER_OFF_HEADER_SIZE 28u
#define ATP_LEDGER_PAYLOAD_MAX 1024u
#define ATP_LEDGER_RECORD_MAX (9u + ATP_LEDGER_PAYLOAD_MAX)

#define ATP_LEDGER_SLOT_EMPTY UINT64_MAX

typedef struct atp_ledger_slot {
    uint64_t key;
    uint64_t entry;
} atp_ledger_slot;

struct atp_ledger {
    char *log_path;
    char *log_tmp_path;
    char *off_path;
    char *off_tmp_path;

    FILE *log;

    atp_ledger_entry *entries; /* indexed by id-1 */
    size_t count;
    size_t capacity;

    uint64_t committed_offset;

    atp_ledger_slot *index;
    size_t index_size;
};

static void atp_store_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8u) & 0xffu);
    out[2] = (unsigned char)((value >> 16u) & 0xffu);
    out[3] = (unsigned char)((value >> 24u) & 0xffu);
}

static void atp_store_u64_le(unsigned char *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) {
        out[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

static uint32_t atp_load_u32_le(const unsigned char *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8u) | ((uint32_t)in[2] << 16u) |
           ((uint32_t)in[3] << 24u);
}

static uint64_t atp_load_u64_le(const unsigned char *in) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        value |= (uint64_t)in[i] << (8u * i);
    }
    return value;
}

static bool atp_fsync(FILE *file) {
    if (fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static bool atp_truncate_file(FILE *file, uint64_t size) {
#if defined(_WIN32)
    if (_chsize_s(_fileno(file), (__int64)size) != 0) {
        return false;
    }
#else
    if (ftruncate(fileno(file), (off_t)size) != 0) {
        return false;
    }
#endif
    return fseek(file, 0, SEEK_END) == 0;
}

static bool atp_read_file(FILE *file, void *data, size_t size) {
    return fread(data, 1u, size, file) == size;
}

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

/* FNV-1a checksum truncated to 32 bits. Deterministic and portable; this is
 * corruption/torn-tail detection, not a security primitive. */
static uint32_t atp_ledger_checksum(const void *data, size_t length) {
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

static bool atp_ledger_outcome_valid(uint8_t value) {
    return value <= (uint8_t)ATP_LEDGER_OUTCOME_FAILED;
}

static bool atp_ledger_is_committed(atp_ledger_outcome outcome) {
    return outcome == ATP_LEDGER_OUTCOME_LEARNED || outcome == ATP_LEDGER_OUTCOME_SKIPPED;
}

static uint64_t atp_ledger_derive_key(const char *source, size_t source_len, uint64_t digest) {
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

static bool atp_ledger_reserve_entries(atp_ledger *ledger, size_t needed) {
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
    ledger->entries = entries;
    ledger->capacity = capacity;
    return true;
}

static bool atp_ledger_index_ensure_capacity(atp_ledger *ledger) {
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

static void atp_ledger_index_insert(atp_ledger *ledger, uint64_t key, size_t entry_index) {
    const size_t mask = ledger->index_size - 1u;
    size_t slot = (size_t)(key & mask);
    while (ledger->index[slot].entry != ATP_LEDGER_SLOT_EMPTY) {
        slot = (slot + 1u) & mask;
    }
    ledger->index[slot] = (atp_ledger_slot){.key = key, .entry = (uint64_t)entry_index};
}

static int64_t atp_ledger_index_find(const atp_ledger *ledger, const char *source_id,
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

static bool atp_write_off(atp_ledger *ledger) {
    FILE *file = fopen(ledger->off_tmp_path, "wb");
    if (!file) {
        return false;
    }
    unsigned char header[ATP_LEDGER_OFF_HEADER_SIZE];
    header[0] = ATP_LEDGER_OFF_MAGIC_0;
    header[1] = ATP_LEDGER_OFF_MAGIC_1;
    header[2] = ATP_LEDGER_OFF_MAGIC_2;
    header[3] = ATP_LEDGER_OFF_MAGIC_3;
    header[4] = ATP_LEDGER_OFF_MAGIC_4;
    header[5] = ATP_LEDGER_OFF_MAGIC_5;
    header[6] = ATP_LEDGER_OFF_MAGIC_6;
    header[7] = ATP_LEDGER_OFF_MAGIC_7;
    atp_store_u32_le(&header[8], ATPERSON_LEDGER_VERSION);
    atp_store_u64_le(&header[12], (uint64_t)ledger->count);
    atp_store_u64_le(&header[20], ledger->committed_offset);

    bool ok = fwrite(header, 1u, sizeof(header), file) == sizeof(header);
    if (!atp_fsync(file)) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(ledger->off_tmp_path);
        return false;
    }
    if (rename(ledger->off_tmp_path, ledger->off_path) != 0) {
        remove(ledger->off_tmp_path);
        return false;
    }
    return true;
}

static bool atp_read_off(atp_ledger *ledger, uint64_t *out_count, uint64_t *out_offset) {
    FILE *file = fopen(ledger->off_path, "rb");
    if (!file) {
        return false;
    }
    unsigned char header[ATP_LEDGER_OFF_HEADER_SIZE];
    const bool ok = fread(header, 1u, sizeof(header), file) == sizeof(header) &&
                    header[0] == ATP_LEDGER_OFF_MAGIC_0 && header[1] == ATP_LEDGER_OFF_MAGIC_1 &&
                    header[2] == ATP_LEDGER_OFF_MAGIC_2 && header[3] == ATP_LEDGER_OFF_MAGIC_3 &&
                    header[4] == ATP_LEDGER_OFF_MAGIC_4 && header[5] == ATP_LEDGER_OFF_MAGIC_5 &&
                    header[6] == ATP_LEDGER_OFF_MAGIC_6 && header[7] == ATP_LEDGER_OFF_MAGIC_7 &&
                    atp_load_u32_le(&header[8]) == ATPERSON_LEDGER_VERSION;
    fclose(file);
    if (!ok) {
        return false;
    }
    *out_count = atp_load_u64_le(&header[12]);
    *out_offset = atp_load_u64_le(&header[20]);
    return true;
}

static size_t atp_serialize_entry(unsigned char *out, const atp_ledger_entry *entry) {
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
    return pos;
}

static bool atp_parse_entry(const unsigned char *payload, size_t length, atp_ledger_entry *out) {
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
    return true;
}

static bool atp_parse_patch(const unsigned char *payload, size_t length, uint64_t *id,
                            uint8_t *outcome) {
    if (length != 9u) {
        return false;
    }
    *id = atp_load_u64_le(payload);
    *outcome = payload[8];
    return true;
}

static size_t atp_build_record(unsigned char record[ATP_LEDGER_RECORD_MAX], uint8_t type,
                               const unsigned char *payload, size_t payload_len) {
    atp_store_u32_le(&record[0], (uint32_t)payload_len);
    atp_store_u32_le(&record[4], atp_ledger_checksum(payload, payload_len));
    record[8] = type;
    memcpy(&record[9], payload, payload_len);
    return 9u + payload_len;
}

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

static atp_status atp_ledger_recover(atp_ledger *ledger) {
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
    unsigned char buffer[ATP_LEDGER_RECORD_MAX];
    while (position < fence) {
        const size_t remaining = (size_t)(fence - position);
        if (remaining < 9u) {
            /* Torn record header. */
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }
        if (!atp_read_file(ledger->log, &buffer[0], 4u) ||
            !atp_read_file(ledger->log, &buffer[4], 4u)) {
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }
        const uint32_t payload_len = atp_load_u32_le(&buffer[0]);
        const uint32_t crc = atp_load_u32_le(&buffer[4]);
        if (payload_len == 0u || payload_len > ATP_LEDGER_PAYLOAD_MAX ||
            9u + (size_t)payload_len > remaining) {
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }
        if (!atp_read_file(ledger->log, &buffer[8], 1u) ||
            !atp_read_file(ledger->log, &buffer[9], payload_len)) {
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }
        if (atp_ledger_checksum(&buffer[9], payload_len) != crc) {
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }

        const uint8_t type = buffer[8];
        const unsigned char *payload = &buffer[9];
        const size_t payload_size = (size_t)payload_len;

        if (type == ATP_LEDGER_RECORD_ENTRY) {
            atp_ledger_entry entry;
            if (!atp_parse_entry(payload, payload_size, &entry)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                return ATP_ERR_FORMAT;
            }
            if (entry.id != (uint64_t)ledger->count + 1u ||
                !atp_ledger_reserve_entries(ledger, ledger->count + 1u)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                return ATP_ERR_FORMAT;
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
                return ATP_ERR_FORMAT;
            }
            if (patch_id == 0u || patch_id > (uint64_t)ledger->count ||
                !atp_ledger_outcome_valid(patch_outcome)) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                return ATP_ERR_FORMAT;
            }
            atp_ledger_entry *entry = &ledger->entries[patch_id - 1u];
            if (atp_ledger_is_committed(entry->outcome) &&
                (atp_ledger_outcome)patch_outcome == ATP_LEDGER_OUTCOME_PENDING) {
                if (self_heal) {
                    fence = position;
                    break;
                }
                return ATP_ERR_FORMAT;
            }
            entry->outcome = (atp_ledger_outcome)patch_outcome;
        } else {
            if (self_heal) {
                fence = position;
                break;
            }
            return ATP_ERR_FORMAT;
        }
        position += 9u + (size_t)payload_len;
    }

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

static void atp_ledger_release(atp_ledger *ledger) {
    if (!ledger) {
        return;
    }
    if (ledger->log) {
        fclose(ledger->log);
    }
    free(ledger->log_path);
    free(ledger->log_tmp_path);
    free(ledger->off_path);
    free(ledger->off_tmp_path);
    free(ledger->entries);
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
    if (fread(header, 1u, sizeof(header), log) != sizeof(header) ||
        header[0] != ATP_LEDGER_FILE_MAGIC_0 || header[1] != ATP_LEDGER_FILE_MAGIC_1 ||
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
        atp_fsync(ledger->log);
        fclose(ledger->log);
    }
    atp_ledger_release(ledger);
}

atp_ledger_result atp_ledger_append(atp_ledger *ledger, const char *source_id,
                                    const char *author_did, uint64_t observed_at,
                                    uint64_t content_digest, uint32_t schema_version,
                                    atp_ledger_outcome outcome, uint64_t *out_id,
                                    atp_status *status) {
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

    unsigned char payload[ATP_LEDGER_PAYLOAD_MAX];
    const size_t payload_len = atp_serialize_entry(payload, &entry);
    unsigned char record[ATP_LEDGER_RECORD_MAX];
    const size_t record_len =
        atp_build_record(record, ATP_LEDGER_RECORD_ENTRY, payload, payload_len);
    /* Increment the count before the durable commit so the offset marker
     * records the post-append count; recover() trusts that fence. */
    ledger->count++;
    const atp_status committed = atp_ledger_commit_record(ledger, record, record_len);
    if (committed != ATP_OK) {
        ledger->count--;
        if (status) {
            *status = committed;
        }
        return ATP_LEDGER_NOT_FOUND;
    }

    ledger->entries[ledger->count - 1u] = entry;
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