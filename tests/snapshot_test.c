#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Snapshot format v5 tests: portability framing, digest verification,
 * corruption rejection, and v4 migration.
 */

static const char *SNAPSHOT_PATH = "atperson-snapshot-test-v5.bin";
static const char *V4_PATH = "atperson-snapshot-test-v4.bin";
static const char *CORRUPT_PATH = "atperson-snapshot-test-corrupt.bin";

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    unsigned char *data = malloc((size_t)length);
    assert(data != NULL);
    assert(fread(data, 1u, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void write_file(const char *path, const unsigned char *data, size_t size) {
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1u, size, file) == size);
    assert(fclose(file) == 0);
}

/* Build a graph with a little of everything: nodes, edges, ledger entries
 * (including a withdrawn one), and an episode. */
static atp_graph *build_sample_graph(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 4242u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "moon light moon", "at://sample/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "moon stone", "at://sample/2") == ATP_OK);
    assert(atp_graph_observe_text(graph, "light lantern", "at://sample/3") == ATP_OK);

    atp_ledger_entry mirror[3] = {0};
    strncpy(mirror[0].source_id, "at://sample/1", sizeof(mirror[0].source_id) - 1u);
    strncpy(mirror[0].author_did, "did:plc:sample", sizeof(mirror[0].author_did) - 1u);
    mirror[0].id = 11u;
    mirror[0].observed_at = 100u;
    mirror[0].content_digest = atp_ledger_digest("moon light moon", 15u);
    mirror[0].schema_version = ATPERSON_SCHEMA_VERSION;
    mirror[0].outcome = ATP_LEDGER_OUTCOME_LEARNED;
    strncpy(mirror[1].source_id, "at://sample/2", sizeof(mirror[1].source_id) - 1u);
    mirror[1].id = 12u;
    mirror[1].observed_at = 200u;
    mirror[1].content_digest = 42u;
    mirror[1].schema_version = ATPERSON_SCHEMA_VERSION;
    mirror[1].outcome = ATP_LEDGER_OUTCOME_SKIPPED;
    strncpy(mirror[2].source_id, "at://sample/3", sizeof(mirror[2].source_id) - 1u);
    strncpy(mirror[2].author_did, "did:plc:sample", sizeof(mirror[2].author_did) - 1u);
    mirror[2].id = 13u;
    mirror[2].observed_at = 300u;
    mirror[2].content_digest = 77u;
    mirror[2].schema_version = ATPERSON_SCHEMA_VERSION;
    mirror[2].outcome = ATP_LEDGER_OUTCOME_WITHDRAWN;
    for (size_t i = 0u; i < 3u; ++i) {
        assert(atp_graph_add_ledger_entry(graph, &mirror[i]) == ATP_OK);
    }
    return graph;
}

static void assert_sample_graph(const atp_graph *graph) {
    const atp_graph_stats stats = atp_graph_get_stats(graph);
    assert(stats.node_count == 4u);
    assert(stats.edge_count == 4u);
    assert(atp_graph_ledger_count(graph) == 3u);

    atp_association association[4];
    size_t association_count = 0u;
    assert(atp_graph_associations(graph, "moon", association, 4u, &association_count) ==
           ATP_OK);
    assert(association_count == 2u);

    /* Ledger state survives, including the withdrawn outcome. */
    atp_ledger_entry entry = {0};
    assert(atp_graph_ledger_entry(graph, 0u, &entry) == ATP_OK);
    assert(entry.id == 11u);
    assert(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    assert(strcmp(entry.author_did, "did:plc:sample") == 0);
    assert(atp_graph_ledger_entry(graph, 2u, &entry) == ATP_OK);
    assert(entry.id == 13u);
    assert(entry.outcome == ATP_LEDGER_OUTCOME_WITHDRAWN);
}

static void test_v5_magic_and_layout(void) {
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);
    /* Magic. */
    assert(size > 32u);
    assert(memcmp(data, "ATPERSN5", 8u) == 0);
    /* Version u32le. */
    assert(data[8] == 5u && data[9] == 0u && data[10] == 0u && data[11] == 0u);
    /* First section header: tag 1 (HEADER), length fits in remaining bytes. */
    uint64_t section_length = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        section_length |= (uint64_t)data[16u + i] << (8u * i);
    }
    assert(section_length > 0u);
    assert(section_length < size);
    free(data);
    remove(SNAPSHOT_PATH);
}

static void test_learning_schema_section(void) {
    /* The schema section records which learning algorithm produced the
     * state. A compatible schema loads; a foreign one is refused with the
     * schema-specific status rather than silently extended. */
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);

    /* Find the schema section (tag 8) and verify it records schema 1. */
    size_t position = 12u;
    bool found = false;
    while (position + 12u <= size - 8u) {
        uint32_t tag = 0u;
        uint64_t length = 0u;
        for (unsigned i = 0u; i < 4u; ++i) {
            tag |= (uint32_t)data[position + i] << (8u * i);
        }
        for (unsigned i = 0u; i < 8u; ++i) {
            length |= (uint64_t)data[position + 4u + i] << (8u * i);
        }
        if (tag == 8u) {
            found = true;
            assert(length == 4u);
            assert(data[position + 12u] == ATPERSON_SCHEMA_VERSION); /* u32le */
            break;
        }
        position += 12u + (size_t)length;
    }
    assert(found);

    /* Rewrite the schema section with a foreign version and fix the
     * digest: the load must fail with ATP_ERR_SCHEMA. */
    data[position + 12u] = ATPERSON_SCHEMA_VERSION + 1u;
    uint64_t digest = 1469598103934665603u;
    for (size_t i = 0u; i < size - 8u; ++i) {
        digest ^= data[i];
        digest *= 1099511628211u;
    }
    for (unsigned i = 0u; i < 8u; ++i) {
        data[size - 8u + i] = (unsigned char)((digest >> (8u * i)) & 0xffu);
    }
    write_file(CORRUPT_PATH, data, size);
    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(CORRUPT_PATH, &status);
    assert(loaded == NULL);
    assert(status == ATP_ERR_SCHEMA);

    free(data);
    remove(SNAPSHOT_PATH);
    remove(CORRUPT_PATH);
}

static void test_roundtrip_stability(void) {
    /* Save, load, save again: the second image must be byte-identical.
     * This pins the encoding (little-endian, section order, digest). */
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    atp_status status = ATP_OK;
    graph = atp_graph_load(SNAPSHOT_PATH, &status);
    assert(graph != NULL);
    assert(status == ATP_OK);
    assert_sample_graph(graph);
    assert(atp_graph_save(graph, V4_PATH) == ATP_OK); /* reused as second path */
    atp_graph_destroy(graph);

    size_t first_size = 0u;
    size_t second_size = 0u;
    unsigned char *first = read_file(SNAPSHOT_PATH, &first_size);
    unsigned char *second = read_file(V4_PATH, &second_size);
    assert(first_size == second_size);
    assert(memcmp(first, second, first_size) == 0);
    free(first);
    free(second);
    remove(SNAPSHOT_PATH);
    remove(V4_PATH);
}

static void test_truncated_rejected(void) {
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);

    /* Every truncation point must be rejected, never crash. */
    for (size_t cut = 1u; cut < size; ++cut) {
        write_file(CORRUPT_PATH, data, cut);
        atp_status status = ATP_OK;
        atp_graph *loaded = atp_graph_load(CORRUPT_PATH, &status);
        assert(loaded == NULL);
        assert(status == ATP_ERR_FORMAT || status == ATP_ERR_IO);
    }
    free(data);
    remove(SNAPSHOT_PATH);
    remove(CORRUPT_PATH);
}

static void test_digest_rejects_bitrot(void) {
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);

    /* Flip one bit in each section region: the digest must catch it. */
    for (size_t offset = 8u; offset < size - 8u; offset += 7u) {
        data[offset] ^= 0x01u;
        write_file(CORRUPT_PATH, data, size);
        atp_status status = ATP_OK;
        atp_graph *loaded = atp_graph_load(CORRUPT_PATH, &status);
        if (loaded) {
            /* The flipped byte was inside a skipped or semantically-neutral
             * region only if the digest still matched, which it cannot;
             * every flip must fail the digest. */
            assert(loaded == NULL);
        }
        assert(status == ATP_ERR_FORMAT);
        data[offset] ^= 0x01u;
    }
    free(data);
    remove(SNAPSHOT_PATH);
    remove(CORRUPT_PATH);
}

static void test_oversized_section_rejected(void) {
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);

    /* Claim the first section is 2^40 bytes long: the reader must refuse
     * before allocating (bounds check against remaining file size). */
    const uint64_t absurd = (uint64_t)1u << 40;
    for (unsigned i = 0u; i < 8u; ++i) {
        data[16u + i] = (unsigned char)((absurd >> (8u * i)) & 0xffu);
    }
    write_file(CORRUPT_PATH, data, size);
    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(CORRUPT_PATH, &status);
    assert(loaded == NULL);
    assert(status == ATP_ERR_FORMAT);

    free(data);
    remove(SNAPSHOT_PATH);
    remove(CORRUPT_PATH);
}

static void test_unknown_section_skipped(void) {
    atp_graph *graph = build_sample_graph();
    assert(atp_graph_save(graph, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);

    /* Splice an unknown section (tag 99) with a small payload before the
     * trailing digest, then recompute the digest. An older reader must
     * skip it and still load the graph. */
    const size_t splice_size = 12u + 4u;
    unsigned char *spliced = malloc(size + splice_size);
    assert(spliced != NULL);
    const size_t digest_offset = size - 8u;
    memcpy(spliced, data, digest_offset);
    /* tag 99 u32le, length 4 u64le, payload "junk" */
    spliced[digest_offset] = 99u;
    for (size_t i = 1u; i < 12u; ++i) {
        spliced[digest_offset + i] = 0u;
    }
    spliced[digest_offset + 4u] = 4u;
    memcpy(spliced + digest_offset + 12u, "junk", 4u);
    memcpy(spliced + digest_offset + splice_size, data + digest_offset, 8u);
    /* Recompute digest over the extended body. */
    uint64_t digest = 1469598103934665603u;
    for (size_t i = 0u; i < digest_offset + splice_size; ++i) {
        digest ^= spliced[i];
        digest *= 1099511628211u;
    }
    for (unsigned i = 0u; i < 8u; ++i) {
        spliced[digest_offset + splice_size + i] =
            (unsigned char)((digest >> (8u * i)) & 0xffu);
    }
    write_file(CORRUPT_PATH, spliced, size + splice_size);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(CORRUPT_PATH, &status);
    assert(loaded != NULL);
    assert(status == ATP_OK);
    assert_sample_graph(loaded);
    atp_graph_destroy(loaded);

    free(spliced);
    free(data);
    remove(SNAPSHOT_PATH);
    remove(CORRUPT_PATH);
}

static void test_v4_migration(void) {
    /* Hand-encode a minimal v4 snapshot: magic ATPERSN1, version 4, host
     * layout as written by the v4 writer (little-endian host). The v5
     * reader must load it and the next save must produce v5 bytes. */
    atp_graph_config config = atp_graph_default_config();
    config.seed = 77u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "moon light", "at://v4/1") == ATP_OK);
    atp_graph_destroy(graph);

    /* Round-trip through the public API cannot produce v4 bytes anymore,
     * so build them from the documented v4 layout directly. */
    FILE *file = fopen(V4_PATH, "wb");
    assert(file != NULL);
    const uint32_t version = 4u;
    const uint32_t embedding_dim = ATPERSON_EMBEDDING_DIM;
    const uint32_t hidden_dim = 16u;
    const uint64_t seed = 77u;
    float learning_rate = config.learning_rate;
    float familiarity_decay = config.familiarity_decay;
    const uint64_t rng_state = 1u;
    const uint64_t observations = 2u;
    const uint64_t token_observations = 2u;
    const uint64_t training_steps = 1u;
    const double loss_total = 0.25;
    const uint64_t node_count = 2u;
    const uint64_t edge_count = 1u;
    assert(fwrite("ATPERSN1", 1u, 8u, file) == 8u);
    assert(fwrite(&version, sizeof(version), 1u, file) == 1u);
    assert(fwrite(&embedding_dim, sizeof(embedding_dim), 1u, file) == 1u);
    assert(fwrite(&hidden_dim, sizeof(hidden_dim), 1u, file) == 1u);
    assert(fwrite(&seed, sizeof(seed), 1u, file) == 1u);
    assert(fwrite(&learning_rate, sizeof(learning_rate), 1u, file) == 1u);
    assert(fwrite(&familiarity_decay, sizeof(familiarity_decay), 1u, file) == 1u);
    assert(fwrite(&rng_state, sizeof(rng_state), 1u, file) == 1u);
    assert(fwrite(&observations, sizeof(observations), 1u, file) == 1u);
    assert(fwrite(&token_observations, sizeof(token_observations), 1u, file) == 1u);
    assert(fwrite(&training_steps, sizeof(training_steps), 1u, file) == 1u);
    assert(fwrite(&loss_total, sizeof(loss_total), 1u, file) == 1u);
    assert(fwrite(&node_count, sizeof(node_count), 1u, file) == 1u);
    assert(fwrite(&edge_count, sizeof(edge_count), 1u, file) == 1u);
    /* Network struct: raw floats (all-zero would do, but write the real
     * layout size). */
    float network_floats[16u * 32u + 16u + 16u + 1u] = {0};
    assert(fwrite(network_floats, sizeof(float), 545u, file) == 545u);
    /* Nodes: len u32, observations u64, embedding raw, token bytes. */
    const uint32_t len_moon = 4u;
    const uint64_t moon_obs = 1u;
    float moon_embedding[ATPERSON_EMBEDDING_DIM] = {0};
    assert(fwrite(&len_moon, sizeof(len_moon), 1u, file) == 1u);
    assert(fwrite(&moon_obs, sizeof(moon_obs), 1u, file) == 1u);
    assert(fwrite(moon_embedding, sizeof(float), ATPERSON_EMBEDDING_DIM, file) ==
           ATPERSON_EMBEDDING_DIM);
    assert(fwrite("moon", 1u, 4u, file) == 4u);
    const uint32_t len_light = 5u;
    const uint64_t light_obs = 1u;
    assert(fwrite(&len_light, sizeof(len_light), 1u, file) == 1u);
    assert(fwrite(&light_obs, sizeof(light_obs), 1u, file) == 1u);
    assert(fwrite(moon_embedding, sizeof(float), ATPERSON_EMBEDDING_DIM, file) ==
           ATPERSON_EMBEDDING_DIM);
    assert(fwrite("light", 1u, 5u, file) == 5u);
    /* Edge: moon -> light. */
    const uint32_t edge_source = 0u;
    const uint32_t edge_target = 1u;
    const uint64_t edge_obs = 1u;
    const uint64_t edge_hash = 0u;
    float edge_strength = 1.0f;
    assert(fwrite(&edge_source, sizeof(edge_source), 1u, file) == 1u);
    assert(fwrite(&edge_target, sizeof(edge_target), 1u, file) == 1u);
    assert(fwrite(&edge_obs, sizeof(edge_obs), 1u, file) == 1u);
    assert(fwrite(&edge_hash, sizeof(edge_hash), 1u, file) == 1u);
    assert(fwrite(&edge_strength, sizeof(edge_strength), 1u, file) == 1u);
    /* Ledger: count 0. */
    const uint64_t ledger_count = 0u;
    assert(fwrite(&ledger_count, sizeof(ledger_count), 1u, file) == 1u);
    /* Episodes: count 0. */
    const uint64_t episode_count = 0u;
    assert(fwrite(&episode_count, sizeof(episode_count), 1u, file) == 1u);
    /* Familiarity block: count u32 == node_count, then one float each. */
    const uint32_t familiarity_count = 2u;
    assert(fwrite(&familiarity_count, sizeof(familiarity_count), 1u, file) == 1u);
    float familiarity = 1.0f;
    assert(fwrite(&familiarity, sizeof(float), 1u, file) == 1u);
    assert(fwrite(&familiarity, sizeof(float), 1u, file) == 1u);
    assert(fclose(file) == 0);

    /* Load the v4 snapshot. */
    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(V4_PATH, &status);
    assert(loaded != NULL);
    assert(status == ATP_OK);
    const atp_graph_stats loaded_stats = atp_graph_get_stats(loaded);
    assert(loaded_stats.node_count == 2u);
    assert(loaded_stats.edge_count == 1u);
    atp_association association[2];
    size_t association_count = 0u;
    assert(atp_graph_associations(loaded, "moon", association, 2u, &association_count) ==
           ATP_OK);
    assert(association_count == 1u);
    assert(strcmp(association[0].token, "light") == 0);

    /* Save: must produce v5 bytes. */
    assert(atp_graph_save(loaded, SNAPSHOT_PATH) == ATP_OK);
    atp_graph_destroy(loaded);

    size_t size = 0u;
    unsigned char *data = read_file(SNAPSHOT_PATH, &size);
    assert(memcmp(data, "ATPERSN5", 8u) == 0);
    assert(data[8] == 5u);
    free(data);
    remove(SNAPSHOT_PATH);
    remove(V4_PATH);
}

static void test_v1_v3_rejected(void) {
    /* v1-v3 shared the ATPERSN1 magic with different layouts; the reader
     * must refuse them by version rather than misparse. */
    for (uint32_t version = 1u; version <= 3u; ++version) {
        FILE *file = fopen(V4_PATH, "wb");
        assert(file != NULL);
        assert(fwrite("ATPERSN1", 1u, 8u, file) == 8u);
        assert(fwrite(&version, sizeof(version), 1u, file) == 1u);
        assert(fclose(file) == 0);

        atp_status status = ATP_OK;
        atp_graph *loaded = atp_graph_load(V4_PATH, &status);
        assert(loaded == NULL);
        assert(status == ATP_ERR_FORMAT);
    }
    remove(V4_PATH);
}

static void test_bad_magic_rejected(void) {
    FILE *file = fopen(V4_PATH, "wb");
    assert(file != NULL);
    const uint32_t version = ATPERSON_SNAPSHOT_VERSION;
    assert(fwrite("ATPERSNX", 1u, 8u, file) == 8u);
    assert(fwrite(&version, sizeof(version), 1u, file) == 1u);
    assert(fclose(file) == 0);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(V4_PATH, &status);
    assert(loaded == NULL);
    assert(status == ATP_ERR_FORMAT);
    remove(V4_PATH);
}

int main(void) {
    assert(ATPERSON_SNAPSHOT_VERSION == 5u);
    test_v5_magic_and_layout();
    test_learning_schema_section();
    test_roundtrip_stability();
    test_truncated_rejected();
    test_digest_rejects_bitrot();
    test_oversized_section_rejected();
    test_unknown_section_skipped();
    test_v4_migration();
    test_v1_v3_rejected();
    test_bad_magic_rejected();
    printf("snapshot_test: all tests passed\n");
    return 0;
}
