#include "persistence/reader.h"

#include <string.h>

/*
 * Bounded primitive reads for snapshot decoders. This module owns no memory;
 * it only advances a cursor over caller-owned bytes. Read failure leaves the
 * caller responsible for discarding any partially decoded graph state.
 */
bool atp_reader_take(atp_reader *reader, size_t count, const unsigned char **out) {
    if (count > reader->size - reader->position) {
        return false;
    }
    *out = reader->data + reader->position;
    reader->position += count;
    return true;
}

bool atp_reader_u32(atp_reader *reader, uint32_t *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 4u, &bytes)) {
        return false;
    }
    *value = atp_load_u32le(bytes);
    return true;
}

bool atp_reader_u64(atp_reader *reader, uint64_t *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 8u, &bytes)) {
        return false;
    }
    *value = atp_load_u64le(bytes);
    return true;
}

bool atp_reader_f32(atp_reader *reader, float *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 4u, &bytes)) {
        return false;
    }
    *value = atp_load_f32le(bytes);
    return true;
}

bool atp_reader_f64(atp_reader *reader, double *value) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 8u, &bytes)) {
        return false;
    }
    *value = atp_load_f64le(bytes);
    return true;
}

bool atp_reader_string(atp_reader *reader, char *out, size_t capacity) {
    uint32_t length = 0u;
    const unsigned char *bytes;
    if (!atp_reader_u32(reader, &length) || length == 0u || length >= capacity ||
        !atp_reader_take(reader, length, &bytes)) {
        return false;
    }
    memcpy(out, bytes, length);
    out[length] = '\0';
    return true;
}

bool atp_reader_string_opt(atp_reader *reader, char *out, size_t capacity) {
    uint32_t length = 0u;
    const unsigned char *bytes;
    if (!atp_reader_u32(reader, &length) || length >= capacity ||
        !atp_reader_take(reader, length, &bytes)) {
        return false;
    }
    memcpy(out, bytes, length);
    out[length] = '\0';
    return true;
}

bool atp_reader_section(atp_reader *reader, atp_section *section) {
    const unsigned char *bytes;
    if (!atp_reader_take(reader, 12u, &bytes) || !atp_load_section(bytes, 12u, section)) {
        return false;
    }
    return section->length <= reader->size - reader->position;
}

atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure) {
    atp_graph_destroy(graph);
    if (status) {
        *status = failure;
    }
    return NULL;
}
