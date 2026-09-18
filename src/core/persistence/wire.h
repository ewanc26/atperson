#ifndef ATPERSON_PERSISTENCE_WIRE_H
#define ATPERSON_PERSISTENCE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Growable little-endian snapshot wire buffer (src/core/persistence/wire.c).
 *
 * Owns an append-only byte buffer plus a framed section writer that patches a
 * section's length field when the section ends. Writers run before any
 * destination file exists, so an allocation failure aborts before I/O.
 *
 * Memory contract: the caller owns and frees `data` with free(). On failure
 * all appends fail closed and the caller releases the buffer unchanged. All
 * fixed-width numbers are written in the portable little-endian layout shared
 * with io/portable.h.
 */
typedef struct atp_buffer {
    unsigned char *data;
    size_t size;
    size_t capacity;
} atp_buffer;

typedef struct atp_section_writer {
    atp_buffer *buffer;
    size_t length_offset;
} atp_section_writer;

bool atp_buffer_put(atp_buffer *buffer, const void *data, size_t size);
bool atp_buffer_u32(atp_buffer *buffer, uint32_t value);
bool atp_buffer_u64(atp_buffer *buffer, uint64_t value);
bool atp_buffer_f32(atp_buffer *buffer, float value);
bool atp_buffer_f64(atp_buffer *buffer, double value);
bool atp_buffer_string(atp_buffer *buffer, const char *text);
bool atp_buffer_string_opt(atp_buffer *buffer, const char *text);

/* Begin a section by writing tag | placeholder length; end patches the real
 * length. Nested sections are not supported. */
bool atp_section_begin(atp_buffer *buffer, atp_section_writer *section, uint32_t tag);
void atp_section_end(const atp_section_writer *section);

#endif