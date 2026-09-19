#include "persistence/persistence_internal.h"

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

bool atp_reader_next_section(atp_reader *reader, bool seen[ATP_SECTION_TRACKED_MAX + 1u], uint32_t max_tag,
                             atp_section *section, size_t *payload_end) {
    if (!atp_reader_section(reader, section)) {
        return false;
    }
    if (section->tag != 0u && section->tag <= max_tag) {
        if (seen[section->tag]) {
            return false;
        }
        seen[section->tag] = true;
    }
    *payload_end = reader->position + (size_t)section->length;
    return true;
}

/* Bit N-1 set for every tag N the loader saw; compared against the loader's
 * required-section mask. */
static uint32_t atp_seen_mask(const bool seen[ATP_SECTION_TRACKED_MAX + 1u]) {
    uint32_t mask = 0u;
    for (uint32_t tag = 1u; tag <= ATP_SECTION_TRACKED_MAX; ++tag) {
        if (seen[tag]) {
            mask |= 1u << (tag - 1u);
        }
    }
    return mask;
}

atp_graph *atp_load_finish(atp_graph *graph, const bool seen[ATP_SECTION_TRACKED_MAX + 1u], uint32_t required_mask,
                           uint32_t learning_schema, atp_reader *reader, atp_status *status) {
    if (!graph || (required_mask & ~atp_seen_mask(seen)) != 0u) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_schema_can_replay(learning_schema)) {
        return atp_load_failure(graph, status, ATP_ERR_SCHEMA);
    }

    /* The entry digest check already verified the trailing bytes; here only
     * the framing is confirmed: exactly the digest remains. */
    if (reader->size - reader->position != 8u) {
        return atp_load_failure(graph, status, ATP_ERR_FORMAT);
    }
    if (!atp_graph_rebuild_indexes(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }
    if (!atp_episode_groups_rebuild(graph)) {
        return atp_load_failure(graph, status, ATP_ERR_OUT_OF_MEMORY);
    }

    if (status) {
        *status = ATP_OK;
    }
    return graph;
}

atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure) {
    atp_graph_destroy(graph);
    if (status) {
        *status = failure;
    }
    return NULL;
}
