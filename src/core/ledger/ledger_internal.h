#ifndef ATPERSON_CORE_LEDGER_INTERNAL_H
#define ATPERSON_CORE_LEDGER_INTERNAL_H

/*
 * Shared internals of the observation ledger modules (ledger.c,
 * ledger_format.c, ledger_off.c, ledger_index.c, ledger_recover.c).
 * Not installed and not part of the public API; every prototype below is
 * internal to src/core and must not leak into include/atperson/.
 */

/*
 * fileno/ftruncate/off_t are POSIX. glibc hides them under strict C23
 * (which defines __STRICT_ANSI__), so request the POSIX API explicitly on
 * non-Windows targets; macOS exposes it by default, which is why a mac build
 * does not trip over this.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "atperson/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ATP_LEDGER_FILE_MAGIC_0 'A'
#define ATP_LEDGER_FILE_MAGIC_1 'T'
#define ATP_LEDGER_FILE_MAGIC_2 'P'
#define ATP_LEDGER_FILE_MAGIC_3 'L'
#define ATP_LEDGER_FILE_MAGIC_4 'D'
#define ATP_LEDGER_FILE_MAGIC_5 'G'
#define ATP_LEDGER_FILE_MAGIC_6 '0'
#define ATP_LEDGER_FILE_MAGIC_7 '2'

#define ATP_LEDGER_OFF_MAGIC_0 'A'
#define ATP_LEDGER_OFF_MAGIC_1 'T'
#define ATP_LEDGER_OFF_MAGIC_2 'P'
#define ATP_LEDGER_OFF_MAGIC_3 'L'
#define ATP_LEDGER_OFF_MAGIC_4 'O'
#define ATP_LEDGER_OFF_MAGIC_5 'F'
#define ATP_LEDGER_OFF_MAGIC_6 '0'
#define ATP_LEDGER_OFF_MAGIC_7 '2'

/* v1 magics, recognised only by the migration path. */
#define ATP_LEDGER_V1_FILE_MAGIC_7 '1'
#define ATP_LEDGER_V1_OFF_MAGIC_7 '1'
#define ATP_LEDGER_V1_VERSION 1u

#define ATP_LEDGER_RECORD_ENTRY 1u
#define ATP_LEDGER_RECORD_PATCH 2u

#define ATP_LEDGER_HEADER_SIZE 12u
#define ATP_LEDGER_OFF_HEADER_SIZE 28u
/* Record payloads (the serialized entry/patch body, not the observation
 * payload) stay small; entry bodies may additionally carry an observation
 * payload up to ATPERSON_LEDGER_PAYLOAD_LIMIT. */
#define ATP_LEDGER_PAYLOAD_MAX 1024u
#define ATP_LEDGER_BODY_MAX (ATP_LEDGER_PAYLOAD_MAX + ATPERSON_LEDGER_PAYLOAD_LIMIT)
#define ATP_LEDGER_RECORD_MAX (9u + ATP_LEDGER_BODY_MAX)

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
    unsigned char **payloads;  /* parallel to entries; NULL slot = no payload */
    size_t *payload_lens;      /* parallel to entries */
    size_t count;
    size_t capacity;

    uint64_t committed_offset;

    atp_ledger_slot *index;
    size_t index_size;
};

/* ledger_format.c: record wire format (little-endian, CRC-framed). */

/* Store a little-endian u32/u64 into out (4/8 bytes). */
void atp_store_u32_le(unsigned char *out, uint32_t value);
void atp_store_u64_le(unsigned char *out, uint64_t value);

/* Load a little-endian u32/u64 from in (4/8 bytes). */
uint32_t atp_load_u32_le(const unsigned char *in);
uint64_t atp_load_u64_le(const unsigned char *in);

/* FNV-1a checksum truncated to 32 bits: corruption/torn-tail detection. */
uint32_t atp_ledger_checksum(const void *data, size_t length);

/* True when value is a defined atp_ledger_outcome. */
bool atp_ledger_outcome_valid(uint8_t value);

/* True when outcome is final (learned/skipped/withdrawn), not pending. */
bool atp_ledger_is_committed(atp_ledger_outcome outcome);

/* Serialize a v2 entry body (optionally carrying payload bytes) into out;
 * returns the body length. out needs ATP_LEDGER_BODY_MAX capacity. */
size_t atp_serialize_entry(unsigned char *out, const atp_ledger_entry *entry,
                           const unsigned char *payload, size_t payload_len);

/* Parse a v2 entry body; on success sets *out and points *out_payload at the
 * embedded observation bytes. False on any bounds/shape violation. */
bool atp_parse_entry(const unsigned char *payload, size_t length, atp_ledger_entry *out,
                     const unsigned char **out_payload, size_t *out_payload_len);

/* Parse a 9-byte patch body into id + outcome; false on wrong length. */
bool atp_parse_patch(const unsigned char *payload, size_t length, uint64_t *id,
                     uint8_t *outcome);

/* Frame a typed record (len | crc | type | body) into record; returns the
 * record length. */
size_t atp_build_record(unsigned char record[ATP_LEDGER_RECORD_MAX], uint8_t type,
                        const unsigned char *payload, size_t payload_len);

/* ledger_off.c: durable commit marker and file primitives. */

/* Flush user-space buffers and fsync the file; false on failure. */
bool atp_fsync(FILE *file);

/* Truncate the file to size and seek to the end; false on failure. */
bool atp_truncate_file(FILE *file, uint64_t size);

/* Read exactly size bytes; false on a short read. */
bool atp_read_file(FILE *file, void *data, size_t size);

/* Stage the commit marker in the .off.tmp file, fsync it, and rename it over
 * the .off path; false (with the staging file removed) on failure. */
bool atp_write_off(atp_ledger *ledger);

/* Read and validate the commit marker into the out_count/out_offset
 * outputs; false on a missing or corrupt marker. */
bool atp_read_off(atp_ledger *ledger, uint64_t *out_count, uint64_t *out_offset);

/* ledger_index.c: in-memory dedup index over (source id + digest). */

/* Derive the dedup index key from source id bytes plus content digest. */
uint64_t atp_ledger_derive_key(const char *source, size_t source_len, uint64_t digest);

/* Grow the entry/payload arrays so at least `needed` entries fit; new payload
 * slots start NULL/0. False on overflow or allocation failure. */
bool atp_ledger_reserve_entries(atp_ledger *ledger, size_t needed);

/* Grow/rehash the index so it holds count+1 keys at <= 70% load; false on
 * allocation failure. */
bool atp_ledger_index_ensure_capacity(atp_ledger *ledger);

/* Insert key -> entry_index into a capacity-ensured index. */
void atp_ledger_index_insert(atp_ledger *ledger, uint64_t key, size_t entry_index);

/* Return the entry index for (source_id, digest), or -1 when absent. */
int64_t atp_ledger_index_find(const atp_ledger *ledger, const char *source_id,
                              uint64_t digest);

/* ledger_recover.c: crash recovery and v1 migration. */

/* Validate the committed prefix record-by-record, truncate a torn tail,
 * heal a missing marker, and rebuild in-memory state. Error status on a
 * corrupt fenced prefix. */
atp_status atp_ledger_recover(atp_ledger *ledger);

/* Migrate a v1 log in place to v2 shape (payload_len 0), streaming through a
 * temp file with rename-swap crash ordering; ATP_OK or an error status. */
atp_status atp_ledger_migrate_v1(atp_ledger *ledger);

#endif
