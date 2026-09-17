/*
 * Tokenization contract tests (issue #8).
 *
 * Two suites:
 *   tokenize        — schema-2 Unicode contract: canonical equivalence,
 *                     case folding, scripts, emoji separators, malformed
 *                     UTF-8, codepoint-boundary truncation, and legacy
 *                     schema-1 dispatch equivalence.
 *   tokenize-graph  — the same tokenizer driving real observation: NFD
 *                     and NFC spellings of the same word land on one
 *                     vocabulary node, malformed input cannot intern, and
 *                     recall finds episodes across normalization forms.
 */

#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------- harness -------- */

#define MAX_TOKENS 64
#define MAX_TOKEN_BYTES 128

typedef struct token_list {
    char tokens[MAX_TOKENS][MAX_TOKEN_BYTES];
    size_t count;
} token_list;

static bool collect(void *userdata, const char *token) {
    token_list *list = userdata;
    if (list->count < MAX_TOKENS && strlen(token) < MAX_TOKEN_BYTES) {
        strcpy(list->tokens[list->count], token);
        list->count++;
    }
    return true;
}

static void tokenize(const char *text, uint32_t schema, token_list *out) {
    memset(out, 0, sizeof(*out));
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    /* atp_tokenize is internal; exercise it through observation and read
     * the interned vocabulary back, which is the durable contract. */
    assert(atp_graph_observe_with_memory(graph, text, "at://test/1", "did:plc:t", 1u, 42u, schema,
                                         1u, NULL) == ATP_OK);
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count <= MAX_TOKENS);
    for (size_t i = 0u; i < stats.node_count && i < MAX_TOKENS; ++i) {
        /* Node order is insertion order in a fresh graph. */
    }
    /* Snapshot exposes nodes through associations; simpler: recall the
     * graph's nodes via the public stats plus one association query per
     * token we expect. For contract tests, assert against expected
     * spellings directly. */
    atp_graph_destroy(graph);
    (void)collect;
}

/* Direct tokenizer access via the internal header. */
#include "internal.h"

static void tokenize_direct(const char *text, uint32_t schema, token_list *out) {
    memset(out, 0, sizeof(*out));
    atp_tokenize(text, schema, collect, out);
}

static void expect_tokens(const token_list *list, const char *const *expected, size_t expected_count,
                          const char *label) {
    if (list->count != expected_count) {
        fprintf(stderr, "FAIL %s: got %zu tokens, expected %zu\n", label, list->count,
                expected_count);
        for (size_t i = 0u; i < list->count; ++i) {
            fprintf(stderr, "  [%zu] \"%s\"\n", i, list->tokens[i]);
        }
        exit(1);
    }
    for (size_t i = 0u; i < expected_count; ++i) {
        if (strcmp(list->tokens[i], expected[i]) != 0) {
            fprintf(stderr, "FAIL %s: token %zu is \"%s\", expected \"%s\"\n", label, i,
                    list->tokens[i], expected[i]);
            exit(1);
        }
    }
}

/* -------- contract tests -------- */

static void test_ascii_unchanged(void) {
    token_list list;
    tokenize_direct("Moon light moon", 2u, &list);
    const char *expected[] = {"moon", "light", "moon"};
    expect_tokens(&list, expected, 3u, "ascii lowercase");
}

static void test_canonical_equivalence(void) {
    /* NFC "cafe" + U+0301 vs precomposed U+00E9 — same token. */
    token_list nfc;
    tokenize_direct("caf\xC3\xA9", 2u, &nfc);
    token_list nfd;
    tokenize_direct("cafe\xCC\x81", 2u, &nfd);
    const char *expected[] = {"caf\xC3\xA9"};
    expect_tokens(&nfc, expected, 1u, "nfc accent");
    expect_tokens(&nfd, expected, 1u, "nfd accent folds to nfc");
}

static void test_case_folding(void) {
    /* Latin, Cyrillic, Greek. */
    token_list list;
    tokenize_direct("\xD0\x93\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4", 2u, &list); /* ГОРОД */
    const char *cyrillic_upper[] = {"\xD0\xB3\xD0\xBE\xD1\x80\xD0\xBE\xD0\xB4"}; /* город */
    expect_tokens(&list, cyrillic_upper, 1u, "cyrillic casefold");

    tokenize_direct("\xCE\x93\xCE\xAC\xCE\xBB\xCE\xB1", 2u, &list); /* Γάλα */
    const char *greek[] = {"\xCE\xB3\xCE\xAC\xCE\xBB\xCE\xB1"}; /* γάλα */
    expect_tokens(&list, greek, 1u, "greek casefold");

    /* Sharp s: full casefold maps U+00DF to "ss". */
    tokenize_direct("STRASSE \xC3\x9F", 2u, &list);
    const char *eszett[] = {"strasse", "ss"};
    expect_tokens(&list, eszett, 2u, "eszett casefold");
}

static void test_kcelvin_and_ligature(void) {
    /* Kelvin sign folds to k under NFKC_Casefold. */
    token_list list;
    tokenize_direct("\xE2\x84\xAA", 2u, &list); /* U+212A KELVIN SIGN */
    const char *kelvin[] = {"k"};
    expect_tokens(&list, kelvin, 1u, "kelvin sign");

    /* fi ligature decomposes under NFKC. */
    tokenize_direct("\xEF\xAC\x81", 2u, &list); /* U+FB01 LATIN SMALL LIGATURE FI */
    const char *ligature[] = {"fi"};
    expect_tokens(&list, ligature, 1u, "fi ligature");
}

static void test_cjk_single_token(void) {
    /* Han and kana are letters: one token, no splitting. */
    token_list list;
    tokenize_direct("\xE6\x9D\xB1\xE4\xBA\xAC", 2u, &list); /* 東京 */
    const char *expected[] = {"\xE6\x9D\xB1\xE4\xBA\xAC"};
    expect_tokens(&list, expected, 1u, "cjk single token");
}

static void test_emoji_are_separators(void) {
    token_list list;
    tokenize_direct("moon\xF0\x9F\x8C\x99light", 2u, &list); /* moon🌙light */
    const char *expected[] = {"moon", "light"};
    expect_tokens(&list, expected, 2u, "emoji separates");

    /* ZWJ family sequence: all separator bytes, no vocabulary. */
    tokenize_direct("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", 2u,
                    &list);
    expect_tokens(&list, NULL, 0u, "zwj sequence yields no tokens");

    /* Skin-tone modifier (Symbol, Modifier). */
    tokenize_direct("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB", 2u, &list);
    expect_tokens(&list, NULL, 0u, "skin tone yields no tokens");
}

static void test_apostrophe_variants(void) {
    /* Typographic apostrophe lumps to ASCII under LUMP. */
    token_list list;
    tokenize_direct("don\xE2\x80\x99t", 2u, &list); /* don't with U+2019 */
    const char *expected[] = {"don't"};
    expect_tokens(&list, expected, 1u, "typographic apostrophe");

    tokenize_direct("don't", 2u, &list);
    const char *ascii[] = {"don't"};
    expect_tokens(&list, ascii, 1u, "ascii apostrophe");
}

static void test_urls_and_handles_split(void) {
    token_list list;
    tokenize_direct("https://ewancroft.uk/@ewan", 2u, &list);
    const char *expected[] = {"https", "ewancroft", "uk", "ewan"};
    expect_tokens(&list, expected, 4u, "url splits");
}

static void test_malformed_utf8(void) {
    /* Lone continuation byte: U+FFFD separator, surrounding text survives. */
    token_list list;
    tokenize_direct("moon\x80light", 2u, &list);
    const char *lone[] = {"moon", "light"};
    expect_tokens(&list, lone, 2u, "lone continuation byte");

    /* Truncated 3-byte sequence at end of input. */
    tokenize_direct("moon\xE2\x80", 2u, &list);
    const char *truncated[] = {"moon"};
    expect_tokens(&list, truncated, 1u, "truncated sequence");

    /* Overlong encoding of '/' (must not become a separator). */
    tokenize_direct("moon\xC0\xAFlight", 2u, &list);
    const char *overlong[] = {"moon", "light"};
    expect_tokens(&list, overlong, 2u, "overlong sequence");
}

static void test_codepoint_boundary_truncation(void) {
    /* Build a token of 3-byte codepoints that exceeds 95 bytes: the cut
     * must land on a codepoint boundary, never mid-sequence. */
    char long_input[512];
    size_t len = 0u;
    while (len + 4u < sizeof(long_input)) {
        long_input[len++] = '\xE6';
        long_input[len++] = '\x9D';
        long_input[len++] = '\xB1'; /* 東 */
    }
    long_input[len] = '\0';

    token_list list;
    tokenize_direct(long_input, 2u, &list);
    assert(list.count == 1u);
    const size_t token_len = strlen(list.tokens[0]);
    assert(token_len <= 95u);
    assert(token_len % 3u == 0u); /* whole codepoints only */
}

static void test_legacy_dispatch(void) {
    /* Schema 1 preserves the byte tokenizer: no case folding beyond ASCII,
     * no normalization, bytes >= 0x80 are token bytes. */
    token_list legacy;
    tokenize_direct("MOON \xC3\x9F", 1u, &legacy);
    const char *expected_legacy[] = {"moon", "\xC3\x9F"};
    expect_tokens(&legacy, expected_legacy, 2u, "legacy eszett untouched");

    /* NFC vs NFD stay distinct tokens under schema 1. */
    token_list nfc;
    tokenize_direct("caf\xC3\xA9", 1u, &nfc);
    token_list nfd;
    tokenize_direct("cafe\xCC\x81", 1u, &nfd);
    assert(strcmp(nfc.tokens[0], nfd.tokens[0]) != 0);

    /* Schema 2 folds them together. */
    token_list nfc2;
    tokenize_direct("caf\xC3\xA9", 2u, &nfc2);
    token_list nfd2;
    tokenize_direct("cafe\xCC\x81", 2u, &nfd2);
    assert(strcmp(nfc2.tokens[0], nfd2.tokens[0]) == 0);
}

/* -------- graph-level tests -------- */

static void test_graph_normalization_identity(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 7u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);

    /* NFD spelling first, then NFC: same word, one node. Episodes need
     * observe_with_memory (observe_text builds vocabulary only). */
    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "cafe\xCC\x81 au lait", "at://test/a", "did:plc:t",
                                         1u, 42u, ATPERSON_SCHEMA_VERSION, 1u, &remembered) ==
           ATP_OK);
    assert(remembered);
    atp_graph_stats after_nfd = atp_graph_get_stats(graph);
    assert(after_nfd.node_count == 3u);

    assert(atp_graph_observe_text(graph, "caf\xC3\xA9", "at://test/b") == ATP_OK);
    atp_graph_stats after_nfc = atp_graph_get_stats(graph);
    assert(after_nfc.node_count == 3u); /* no new node */

    /* Recall with either spelling finds the episode. */
    atp_episode episodes[4];
    size_t count = 0u;
    assert(atp_graph_recall(graph, "CAF\xC3\x89", 0u, NULL, NULL, episodes, 4u, &count) == ATP_OK);
    assert(count >= 1u);

    atp_graph_destroy(graph);
}

static void test_graph_malformed_cannot_intern(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    /* Only malformed bytes: no vocabulary, no crash. */
    assert(atp_graph_observe_text(graph, "\x80\xC0\xAF", "at://test/m") == ATP_OK);
    atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 0u);

    atp_graph_destroy(graph);
}

static void test_graph_recall_normalization(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);

    bool remembered = false;
    assert(atp_graph_observe_with_memory(graph, "werewolf moon", "at://test/r", "did:plc:t", 1u, 43u,
                                         ATPERSON_SCHEMA_VERSION, 2u, &remembered) == ATP_OK);
    assert(remembered);

    /* Uppercase and typographic variants still recall. */
    atp_episode episodes[4];
    size_t count = 0u;
    assert(atp_graph_recall(graph, "WEREWOLF", 0u, NULL, NULL, episodes, 4u, &count) == ATP_OK);
    assert(count >= 1u);

    atp_graph_destroy(graph);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <suite>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "tokenize") == 0) {
        test_ascii_unchanged();
        test_canonical_equivalence();
        test_case_folding();
        test_kcelvin_and_ligature();
        test_cjk_single_token();
        test_emoji_are_separators();
        test_apostrophe_variants();
        test_urls_and_handles_split();
        test_malformed_utf8();
        test_codepoint_boundary_truncation();
        test_legacy_dispatch();
        puts("tokenize: ok");
        return 0;
    }
    if (strcmp(argv[1], "tokenize-graph") == 0) {
        test_graph_normalization_identity();
        test_graph_malformed_cannot_intern();
        test_graph_recall_normalization();
        puts("tokenize-graph: ok");
        return 0;
    }

    fprintf(stderr, "unknown suite %s\n", argv[1]);
    return 1;
}
