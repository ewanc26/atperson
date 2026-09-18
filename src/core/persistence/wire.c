#include "persistence/persistence_internal.h"

/*
 * Wire buffer contract: growable little-endian writes and framed sections
 * shared by the v5 and v6 snapshot encoders. No codec policy lives here;
 * this module only turns primitives into bytes.
 */

static bool atp_buffer_reserve(atp_buffer *buffer, size_t needed) {
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    unsigned char *grown = realloc(buffer->data, capacity);
    if (!grown) {
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

bool atp_buffer_put(atp_buffer *buffer, const void *data, size_t size) {
    if (!atp_buffer_reserve(buffer, buffer->size + size)) {
        return false;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return true;
}

bool atp_buffer_u32(atp_buffer *buffer, uint32_t value) {
    unsigned char encoded[4];
    atp_store_u32le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

bool atp_buffer_u64(atp_buffer *buffer, uint64_t value) {
    unsigned char encoded[8];
    atp_store_u64le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

bool atp_buffer_f32(atp_buffer *buffer, float value) {
    unsigned char encoded[4];
    atp_store_f32le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

bool atp_buffer_f64(atp_buffer *buffer, double value) {
    unsigned char encoded[8];
    atp_store_f64le(encoded, value);
    return atp_buffer_put(buffer, encoded, sizeof(encoded));
}

bool atp_buffer_string(atp_buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    if (length == 0u || length > UINT32_MAX) {
        return false;
    }
    return atp_buffer_u32(buffer, (uint32_t)length) && atp_buffer_put(buffer, text, length);
}

bool atp_buffer_string_opt(atp_buffer *buffer, const char *text) {
    const size_t length = strlen(text);
    if (length > UINT32_MAX) {
        return false;
    }
    return atp_buffer_u32(buffer, (uint32_t)length) && atp_buffer_put(buffer, text, length);
}

bool atp_section_begin(atp_buffer *buffer, atp_section_writer *section, uint32_t tag) {
    unsigned char header[12];
    section->buffer = buffer;
    section->length_offset = buffer->size + 4u;
    atp_store_section(header, tag, 0u);
    return atp_buffer_put(buffer, header, sizeof(header));
}

void atp_section_end(const atp_section_writer *section) {
    unsigned char encoded[8];
    const uint64_t length = (uint64_t)(section->buffer->size - section->length_offset - 8u);
    atp_store_u64le(encoded, length);
    memcpy(section->buffer->data + section->length_offset, encoded, sizeof(encoded));
}