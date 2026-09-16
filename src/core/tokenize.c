/*
 * Tokenization contract (issue #8).
 *
 * Token identity is durable learning state: interned vocabulary, edges,
 * episodes, and snapshots all key on token bytes. The contract below is
 * deterministic and versioned through the learning schema.
 *
 * Schema 1 (legacy): byte-oriented. ASCII is lowercased, any byte >= 0x80
 * is a token byte, and tokens cap at ATPERSON_TOKEN_BYTES - 1 bytes with
 * silent truncation. Preserved byte-for-byte so schema-1 ledger entries
 * replay identically.
 *
 * Schema 2 (Unicode): input is first sanitized — invalid UTF-8 sequences
 * become U+FFFD, which the category scan treats as a separator, so
 * malformed bytes can never enter the vocabulary and never pass through
 * raw either (utf8proc_map rejects invalid UTF-8, so sanitization runs
 * before it). The sanitized text is
 * normalized with NFKC_Casefold + LUMP (utf8proc), giving canonical
 * equivalence: NFC and NFD spellings, case variants, ligatures, and
 * typographic variants collapse to one token. Token bytes are then
 * selected by Unicode general category:
 *   - L* (letters), M* (marks), N* (numbers): token bytes
 *   - '_' '\'' '-': token bytes (identifiers, contractions, hyphenations)
 *   - everything else (punctuation, symbols, emoji, whitespace, control,
 *     U+FFFD): separators
 * Emoji are separators on purpose: ZWJ sequences and skin-tone modifiers
 * would otherwise explode the vocabulary with visually-identical
 * variants. Combining marks stay token bytes so scripts without
 * precomposed forms survive normalization.
 *
 * Truncation happens at a codepoint boundary: a token longer than
 * ATPERSON_TOKEN_BYTES - 1 bytes is cut after the last codepoint that
 * fits, never mid-sequence.
 *
 * The tokenizer allocates one scratch buffer per call (utf8proc_map);
 * callers get tokens through a callback so no fixed token count limits
 * the input length.
 */

#include "internal.h"

#include <stdlib.h>
#include <string.h>

#include <utf8proc.h>

/* Legacy schema-1 scanner, preserved byte-for-byte. */
static void atp_tokenize_legacy(const char *text,
                                bool (*emit)(void *userdata, const char *token),
                                void *userdata) {
    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;

    for (const unsigned char *cursor = (const unsigned char *)text;; ++cursor) {
        const unsigned char byte = *cursor;
        const bool token_byte =
            byte != '\0' && (byte >= 0x80u || (byte >= 'a' && byte <= 'z') ||
                             (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                             byte == '\'' || byte == '-' || byte == '_');

        if (token_byte) {
            if (token_len + 1u < sizeof(token)) {
                char lowered = (char)byte;
                if (byte >= 'A' && byte <= 'Z') {
                    lowered = (char)(byte - 'A' + 'a');
                }
                token[token_len++] = lowered;
            }
        }

        if ((!token_byte || byte == '\0') && token_len > 0u) {
            token[token_len] = '\0';
            if (!emit(userdata, token)) {
                return;
            }
            token_len = 0u;
        }

        if (byte == '\0') {
            break;
        }
    }
}

/* True when the codepoint is a token byte under schema 2. */
static bool atp_codepoint_is_token(utf8proc_int32_t codepoint) {
    const utf8proc_property_t *prop = utf8proc_get_property(codepoint);
    if (!prop) {
        return false;
    }
    const utf8proc_category_t category = prop->category;
    const bool letter = category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO;
    const bool mark = category >= UTF8PROC_CATEGORY_MN && category <= UTF8PROC_CATEGORY_ME;
    const bool number = category >= UTF8PROC_CATEGORY_ND && category <= UTF8PROC_CATEGORY_NO;
    return letter || mark || number;
}

/*
 * Sanitize UTF-8 in place: every invalid byte (lone continuation, truncated
 * or overlong sequence, out-of-range codepoint) becomes one U+FFFD, which
 * the category scan treats as a separator. utf8proc_map rejects invalid
 * UTF-8 outright (UTF8PROC_ERROR_INVALIDUTF8), so sanitization must run
 * first — malformed bytes can then never enter the vocabulary and never
 * silently pass through either.
 */
static utf8proc_uint8_t *atp_sanitize_utf8(const char *text) {
    const size_t text_len = strlen(text);
    /* Worst case: every byte invalid -> one U+FFFD (3 bytes) per input byte. */
    utf8proc_uint8_t *out = malloc(text_len * 3u + 1u);
    if (!out) {
        return NULL;
    }

    const utf8proc_uint8_t *cursor = (const utf8proc_uint8_t *)text;
    const utf8proc_uint8_t *end = cursor + text_len;
    size_t out_len = 0u;

    static const utf8proc_uint8_t replacement[3] = {0xEFu, 0xBFu, 0xBDu}; /* U+FFFD */

    while (cursor < end) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t step = utf8proc_iterate(cursor, (utf8proc_ssize_t)(end - cursor),
                                                       &codepoint);
        if (step < 1 || codepoint < 0) {
            memcpy(out + out_len, replacement, sizeof(replacement));
            out_len += sizeof(replacement);
            ++cursor; /* skip exactly the offending byte */
            continue;
        }
        memcpy(out + out_len, cursor, (size_t)step);
        out_len += (size_t)step;
        cursor += step;
    }
    out[out_len] = '\0';
    return out;
}

static void atp_tokenize_unicode(const char *text,
                                 bool (*emit)(void *userdata, const char *token),
                                 void *userdata) {
    utf8proc_uint8_t *sanitized = atp_sanitize_utf8(text);
    if (!sanitized) {
        /* Allocation failure: emit nothing rather than learn from
         * unsanitized bytes. The observation is lost, never corrupted. */
        return;
    }

    utf8proc_uint8_t *normalized = NULL;
    const utf8proc_ssize_t mapped = utf8proc_map(
        sanitized, 0, &normalized,
        (utf8proc_option_t)(UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE |
                            UTF8PROC_COMPAT | UTF8PROC_CASEFOLD | UTF8PROC_IGNORE | UTF8PROC_LUMP));
    free(sanitized);
    if (mapped < 0 || !normalized) {
        /* Unreachable after sanitization (map only fails on invalid UTF-8
         * or allocation failure); fail closed — no tokens, no corruption. */
        return;
    }

    char token[ATPERSON_TOKEN_BYTES];
    size_t token_len = 0u;

    const utf8proc_uint8_t *cursor = normalized;
    while (*cursor) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t step = utf8proc_iterate(cursor, -1, &codepoint);
        if (step < 1 || codepoint == -1) {
            /* Unreachable after a successful map, but never advance blindly. */
            ++cursor;
            continue;
        }

        const bool token_byte = atp_codepoint_is_token(codepoint) || codepoint == '_' ||
                                codepoint == '\'' || codepoint == '-';

        if (token_byte) {
            /* Truncate at the codepoint boundary: append only if the whole
             * sequence fits with room for the terminator. */
            if (token_len + (size_t)step < sizeof(token)) {
                memcpy(token + token_len, cursor, (size_t)step);
                token_len += (size_t)step;
            }
        }

        if (!token_byte && token_len > 0u) {
            token[token_len] = '\0';
            if (!emit(userdata, token)) {
                free(normalized);
                return;
            }
            token_len = 0u;
        }

        cursor += step;
    }

    if (token_len > 0u) {
        token[token_len] = '\0';
        emit(userdata, token);
    }
    free(normalized);
}

void atp_tokenize(const char *text, uint32_t schema_version,
                  bool (*emit)(void *userdata, const char *token), void *userdata) {
    if (!text || !emit) {
        return;
    }
    if (schema_version <= 1u) {
        atp_tokenize_legacy(text, emit, userdata);
    } else {
        atp_tokenize_unicode(text, emit, userdata);
    }
}
