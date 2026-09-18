#ifndef ATPERSON_PERSISTENCE_READER_H
#define ATPERSON_PERSISTENCE_READER_H

#include "internal.h"
#include "io/portable.h"

#include <stddef.h>

/*
 * Bounded snapshot byte reader shared by version-specific decoders.
 * It never allocates and never advances past the supplied image. Callers own
 * the backing bytes and any graph allocations performed after successful reads.
 */
typedef struct atp_reader {
    const unsigned char *data;
    size_t size;
    size_t position;
} atp_reader;

bool atp_reader_take(atp_reader *reader, size_t count, const unsigned char **out);
bool atp_reader_u32(atp_reader *reader, uint32_t *value);
bool atp_reader_u64(atp_reader *reader, uint64_t *value);
bool atp_reader_f32(atp_reader *reader, float *value);
bool atp_reader_f64(atp_reader *reader, double *value);
bool atp_reader_string(atp_reader *reader, char *out, size_t capacity);
bool atp_reader_string_opt(atp_reader *reader, char *out, size_t capacity);
bool atp_reader_section(atp_reader *reader, atp_section *section);

/*
 * Section-walk scaffolding shared by the v5 and v6 loaders. `seen` is a
 * caller-allocated tag table (12 entries, covering every defined section
 * tag); tags beyond `max_tag` and tag 0 never mark an entry. Duplicate
 * known tags fail. `payload_end` receives the exclusive end of the section
 * payload so loaders can skip unknown tags and verify exact consumption.
 */
bool atp_reader_next_section(atp_reader *reader, bool seen[12], uint32_t max_tag,
                             atp_section *section, size_t *payload_end);

/*
 * Shared loader epilogue: required-section mask (`required_mask` bit N-1 set
 * means tag N must have been seen), schema replayability, trailing digest
 * framing, and index/episode-group rebuilds. Destroys the graph and returns
 * NULL with `*status` set on any failure.
 */
atp_graph *atp_load_finish(atp_graph *graph, const bool seen[12], uint32_t required_mask,
                           uint32_t learning_schema, atp_reader *reader, atp_status *status);

atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure);

#endif
