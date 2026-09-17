/* Fuzz target: tokenizer/observation ingestion (issue #11).
 *
 * The tokenizer processes untrusted network text under two versioned
 * contracts (schema 1 byte-oriented, schema 2 Unicode with NFKC_Casefold
 * + LUMP normalization and category-based token selection). This target
 * drives both schemas over arbitrary byte sequences:
 *
 * - schema 2 sanitizes invalid UTF-8 to U+FFFD, which is a separator, so
 *   malformed bytes can never enter the vocabulary;
 * - token truncation happens at codepoint boundaries, never mid-sequence;
 * - emitted tokens are bounded by ATPERSON_TOKEN_BYTES.
 *
 * Every emitted token is checked against the contract, and the same
 * input is also fed through full graph observation so the ingest path
 * (interning, edges, episode selection) sees the tokens too. */

#include "atperson/core.h"
#include "internal.h" /* atp_tokenize is core-private */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool violated;
    bool unicode_schema; /* schema 2 checks only */
} token_contract;

static bool check_token(void *userdata, const char *token) {
    token_contract *contract = userdata;
    const size_t len = strlen(token);
    if (len == 0u || len >= ATPERSON_TOKEN_BYTES) {
        contract->violated = true;
    }
    /* Schema 2 only: U+FFFD is a separator and can never be inside a
     * token. Schema 1 is deliberately byte-oriented — bytes >= 0x80 are
     * token bytes, so U+FFFD's encoding legitimately forms a token there
     * and must be preserved byte-for-byte. */
    if (contract->unicode_schema && strstr(token, "\xEF\xBF\xBD") != NULL) {
        contract->violated = true;
    }
    /* The emit contract returns false to stop; keep scanning. */
    return true;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    /* atp_tokenize takes a NUL-terminated string; the input is copied so
     * an embedded NUL simply ends the text early, matching how the
     * network layer would treat a truncated record. */
    char *text = malloc(size + 1u);
    if (!text) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';

    for (uint32_t schema = 1u; schema <= 2u; ++schema) {
        token_contract contract = {false, schema == 2u};
        atp_tokenize(text, schema, check_token, &contract);
        if (contract.violated) {
            abort(); /* contract violation is a finding */
        }
    }

    /* Full ingestion path: observe the text on a throwaway graph. */
    atp_graph *graph = atp_graph_create(NULL);
    if (graph) {
        (void)atp_graph_observe_text(graph, text, "at://fuzz/source");
        atp_graph_destroy(graph);
    }

    free(text);
    return 0;
}
