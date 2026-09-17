#ifndef ATPERSON_TOKENIZE_H
#define ATPERSON_TOKENIZE_H

#include "atperson/core.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared tokenizer (src/core/tokenize.c). Calls emit once per token; emit
 * returning false stops the scan early. schema_version selects the
 * contract: 1 = legacy byte tokenizer, >= 2 = the Unicode tokenization
 * contract (invalid UTF-8 sanitizes to U+FFFD, NFKC_Casefold + LUMP
 * normalization, category-based token boundaries; see tokenize.c's header
 * comment). All token-producing paths — observation, action context,
 * recall, lookup — go through this one entry point; the runtime uses it
 * through this public declaration so no second scanner copy exists.
 *
 * Read-only: tokenization never mutates the graph or interns vocabulary.
 * The emitted token string is valid only for the duration of the emit call;
 * callers that need to keep it copy it.
 */
void atp_tokenize(const char *text, uint32_t schema_version,
                  bool (*emit)(void *userdata, const char *token), void *userdata);

#ifdef __cplusplus
}
#endif

#endif
