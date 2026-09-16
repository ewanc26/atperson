#ifndef ATPERSON_CORE_PORTABLE_IO_H
#define ATPERSON_CORE_PORTABLE_IO_H

#include "atperson/core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Portable binary IO primitives for the snapshot format.
 *
 * Snapshot v5 is an explicitly portable format: multi-byte integers are
 * little-endian on the wire regardless of host byte order, and floats are
 * IEEE 754 bit patterns. These helpers are the only sanctioned way to
 * encode snapshot fields, so portability is enforced in one place.
 */

/* Snapshot v5 section framing. A section is `tag u32le | length u64le |
 * payload`. Readers bounds-check `length` against the remaining file size
 * before allocating, and skip unknown tags so the format can grow without
 * breaking older readers on newer files. */
typedef struct atp_section {
    uint32_t tag;
    uint64_t length;
} atp_section;

/* Little-endian integer encoding. */
void atp_store_u32le(unsigned char *out, uint32_t value);
void atp_store_u64le(unsigned char *out, uint64_t value);
uint32_t atp_load_u32le(const unsigned char *in);
uint64_t atp_load_u64le(const unsigned char *in);

/* IEEE 754 bit-pattern float encoding: the wire representation is the
 * value's bit pattern stored little-endian. */
void atp_store_f32le(unsigned char *out, float value);
void atp_store_f64le(unsigned char *out, double value);
float atp_load_f32le(const unsigned char *in);
double atp_load_f64le(const unsigned char *in);

/* FNV-1a 64 digest over a byte range (same function family as the ledger). */
uint64_t atp_fnv1a64(const void *data, size_t length);

/* Section header encode/decode. */
void atp_store_section(unsigned char *out, uint32_t tag, uint64_t length);
/* Decode a section header from `in`; returns false when `in` is shorter
 * than the 12-byte header. */
bool atp_load_section(const unsigned char *in, size_t available, atp_section *out);

#endif
