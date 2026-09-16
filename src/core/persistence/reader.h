#ifndef ATPERSON_PERSISTENCE_READER_H
#define ATPERSON_PERSISTENCE_READER_H

#include "internal.h"
#include "portable_io.h"

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

atp_graph *atp_load_failure(atp_graph *graph, atp_status *status, atp_status failure);

#endif
