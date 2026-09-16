#ifndef ATPERSON_CORE_LEDGER_INTERNAL_H
#define ATPERSON_CORE_LEDGER_INTERNAL_H

/*
 * Shared internals of the observation-ledger scope.
 *
 * lifecycle.c owns open/destroy and path lifetime; commit.c owns the durable
 * record-then-marker write primitive; append.c owns new observation commits;
 * outcome.c owns append-only outcome patches/withdrawal; query.c owns
 * read-only inspection; compact.c owns generation compaction; format.c owns
 * record encoding; off.c owns commit markers/file primitives; index.c owns
 * the in-memory dedup index; recover.c owns crash recovery and v1 migration.
 *
 * This header is scope-private and must not leak into include/atperson/.
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

#define ATP_LEDGER_V1_FILE_MAGIC_7 '1'
#define ATP_LEDGER_V1_OFF_MAGIC_7 '1'
#define ATP_LEDGER_V1_VERSION 1u

#define ATP_LEDGER_RECORD_ENTRY 1u
#define ATP_LEDGER_RECORD_PATCH 2u

#define ATP_LEDGER_HEADER_SIZE 12u
#define ATP_LEDGER_OFF_HEADER_SIZE 28u
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

    atp_ledger_entry *entries;
    unsigned char **payloads;
    size_t *payload_lens;
    size_t count;
    size_t capacity;

    uint64_t committed_offset;

    atp_ledger_slot *index;
    size_t index_size;
};

/* commit.c: durable record commit ordering. */
atp_status atp_ledger_commit_record(atp_ledger *ledger, const unsigned char *record,
                                    size_t record_len);

/* format.c: record wire format (little-endian, CRC-framed). */
void atp_store_u32_le(unsigned char *out, uint32_t value);
void atp_store_u64_le(unsigned char *out, uint64_t value);
uint32_t atp_load_u32_le(const unsigned char *in);
uint64_t atp_load_u64_le(const unsigned char *in);
uint32_t atp_ledger_checksum(const void *data, size_t length);
bool atp_ledger_outcome_valid(uint8_t value);
bool atp_ledger_is_committed(atp_ledger_outcome outcome);
size_t atp_serialize_entry(unsigned char *out, const atp_ledger_entry *entry,
                           const unsigned char *payload, size_t payload_len);
bool atp_parse_entry(const unsigned char *payload, size_t length, atp_ledger_entry *out,
                     const unsigned char **out_payload, size_t *out_payload_len);
bool atp_parse_patch(const unsigned char *payload, size_t length, uint64_t *id,
                     uint8_t *outcome);
size_t atp_build_record(unsigned char record[ATP_LEDGER_RECORD_MAX], uint8_t type,
                        const unsigned char *payload, size_t payload_len);

/* off.c: durable commit marker and file primitives. */
bool atp_fsync(FILE *file);
bool atp_truncate_file(FILE *file, uint64_t size);
bool atp_read_file(FILE *file, void *data, size_t size);
bool atp_write_off(atp_ledger *ledger);
bool atp_read_off(atp_ledger *ledger, uint64_t *out_count, uint64_t *out_offset);

/* index.c: in-memory dedup index over (source id + digest). */
uint64_t atp_ledger_derive_key(const char *source, size_t source_len, uint64_t digest);
bool atp_ledger_reserve_entries(atp_ledger *ledger, size_t needed);
bool atp_ledger_index_ensure_capacity(atp_ledger *ledger);
void atp_ledger_index_insert(atp_ledger *ledger, uint64_t key, size_t entry_index);
int64_t atp_ledger_index_find(const atp_ledger *ledger, const char *source_id,
                              uint64_t digest);

/* recover.c: crash recovery and v1 migration. */
atp_status atp_ledger_recover(atp_ledger *ledger);
atp_status atp_ledger_migrate_v1(atp_ledger *ledger);

#endif
