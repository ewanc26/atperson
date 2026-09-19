#include "atperson/core.h"
#include "files.h"
#include "io/portable.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Snapshot format v6 topology tests (issue #72): variable multi-layer
 * architectures round-trip exactly — including plasticity-importance
 * values — legacy graphs keep saving byte-identical v5, v5 files load
 * through the legacy path, and corrupted v6 images fail closed.
 */

static const char *V6_PATH = "atperson-snapshot-test-v6.bin";
static const char *V5_PATH = "atperson-snapshot-test-v5-legacy.bin";
static const char *CORRUPT_V6_PATH = "atperson-snapshot-test-v6-corrupt.bin";

/* One-hidden-layer non-legacy width: same depth, different hidden size, so
 * the graph is non-legacy and must save as v6. */
static atp_neural_architecture one_hidden(uint32_t hidden) {
    atp_neural_architecture architecture = {0};
    architecture.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    architecture.embedding_dim = ATPERSON_EMBEDDING_DIM;
    architecture.input_dim = ATPERSON_EMBEDDING_DIM * 2u;
    architecture.hidden_layer_count = 1u;
    architecture.hidden_widths[0] = hidden;
    architecture.output_dim = 1u;
    return architecture;
}

static atp_neural_architecture three_hidden(void) {
    atp_neural_architecture architecture = one_hidden(5u);
    architecture.hidden_layer_count = 3u;
    architecture.hidden_widths[0] = 6u;
    architecture.hidden_widths[1] = 5u;
    architecture.hidden_widths[2] = 4u;
    return architecture;
}

static atp_graph *graph_at(const atp_neural_architecture *architecture) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 77u;
    atp_graph *graph = atp_graph_create_with_architecture(&config, architecture);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "moon light moon", "at://v6/1") == ATP_OK);
    assert(atp_graph_observe_text(graph, "light lantern", "at://v6/2") == ATP_OK);
    return graph;
}

/* The persisted architecture must equal the requested one field-by-field. */
static void assert_architecture_matches(const atp_graph *graph,
                                        const atp_neural_architecture *expected) {
    atp_neural_architecture_report report = {0};
    assert(atp_graph_neural_report(graph, &report) == ATP_OK);
    assert(report.architecture.version == expected->version);
    assert(report.architecture.embedding_dim == expected->embedding_dim);
    assert(report.architecture.input_dim == expected->input_dim);
    assert(report.architecture.output_dim == expected->output_dim);
    assert(report.architecture.hidden_layer_count == expected->hidden_layer_count);
    for (uint32_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        assert(report.architecture.hidden_widths[i] == expected->hidden_widths[i]);
    }
}

/* Round-trip a trained graph at `architecture` and require the loaded state
 * to reproduce the original exactly: same architecture, same learned
 * scores, and a save of the loaded graph byte-identical to the first save
 * (which also pins the importance arrays — they feed the same encoding). */
static void roundtrip_topology(const atp_neural_architecture *architecture) {
    atp_graph *graph = graph_at(architecture);
    assert(atp_graph_save(graph, V6_PATH) == ATP_OK);

    size_t original_size = 0u;
    unsigned char *original = read_file(V6_PATH, &original_size);
    /* v6 magic and version. */
    assert(original_size > 32u);
    assert(memcmp(original, "ATPERSN6", 8u) == 0);
    assert(original[8] == 6u && original[9] == 0u && original[10] == 0u && original[11] == 0u);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(V6_PATH, &status);
    assert(loaded != NULL);
    assert(status == ATP_OK);
    assert_architecture_matches(loaded, architecture);

    /* Same learned behaviour after the round-trip. */
    const float original_score = atp_graph_familiarity(graph, "moon");
    const float loaded_score = atp_graph_familiarity(loaded, "moon");
    assert(original_score == loaded_score);
    assert(original_score > 0.0f);

    /* Save of the loaded graph is byte-identical: the full trained state,
     * including the plasticity-importance values, survived the round-trip. */
    assert(atp_graph_save(loaded, V6_PATH) == ATP_OK);
    size_t reloaded_size = 0u;
    unsigned char *reloaded = read_file(V6_PATH, &reloaded_size);
    assert(reloaded_size == original_size);
    assert(memcmp(original, reloaded, original_size) == 0);
    free(original);
    free(reloaded);

    atp_graph_destroy(graph);
    atp_graph_destroy(loaded);
    remove(V6_PATH);
}

static void test_roundtrip_one_hidden(void) {
    const atp_neural_architecture architecture = one_hidden(9u);
    roundtrip_topology(&architecture);
}

static void test_roundtrip_two_hidden(void) {
    atp_neural_architecture architecture = one_hidden(6u);
    architecture.hidden_layer_count = 2u;
    architecture.hidden_widths[1] = 3u;
    roundtrip_topology(&architecture);
}

static void test_roundtrip_three_hidden(void) {
    const atp_neural_architecture architecture = three_hidden();
    roundtrip_topology(&architecture);
}

/* Legacy graphs keep saving byte-identical v5 and load as legacy. */
static void test_legacy_saves_v5(void) {
    atp_graph_config config = atp_graph_default_config();
    config.seed = 4242u;
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "moon light moon", "at://legacy/1") == ATP_OK);
    assert(atp_graph_save(graph, V5_PATH) == ATP_OK);

    size_t size = 0u;
    unsigned char *data = read_file(V5_PATH, &size);
    assert(memcmp(data, "ATPERSN5", 8u) == 0);
    assert(data[8] == 5u && data[9] == 0u && data[10] == 0u && data[11] == 0u);
    free(data);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(V5_PATH, &status);
    assert(loaded != NULL);
    assert(status == ATP_OK);
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert_architecture_matches(loaded, &legacy);

    atp_graph_destroy(graph);
    atp_graph_destroy(loaded);
    remove(V5_PATH);
}

/* A v6 image with a corrupted ARCH payload must fail as FORMAT, never
 * produce a partially-created graph. */
static void test_corrupt_arch_rejected(void) {
    const atp_neural_architecture architecture = three_hidden();
    atp_graph *graph = graph_at(&architecture);
    assert(atp_graph_save(graph, CORRUPT_V6_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(CORRUPT_V6_PATH, &size);

    /* Locate the ARCH section (tag 11) and flip its hidden_layer_count. */
    size_t position = 12u;
    int found = 0;
    while (position + 12u <= size - 8u) {
        const uint32_t tag = (uint32_t)data[position] | ((uint32_t)data[position + 1u] << 8u) |
                              ((uint32_t)data[position + 2u] << 16u) |
                              ((uint32_t)data[position + 3u] << 24u);
        uint64_t length = 0u;
        for (unsigned i = 0u; i < 8u; ++i) {
            length |= (uint64_t)data[position + 4u + i] << (8u * i);
        }
        if (tag == 11u) {
            /* hidden_layer_count sits after version/embedding/input/output
             * (4 x u32) at payload offset 16. */
            assert(length >= 20u);
            data[position + 12u + 16u] = 0xFFu;
            found = 1;
            break;
        }
        position += 12u + (size_t)length;
    }
    assert(found == 1);

    /* Refresh the trailing digest so the failure is attributed to the ARCH
     * validation, not bitrot. */
    const uint64_t digest = atp_fnv1a64(data, size - 8u);
    for (unsigned i = 0u; i < 8u; ++i) {
        data[size - 8u + i] = (unsigned char)(digest >> (8u * i));
    }

    write_file(CORRUPT_V6_PATH, data, size);
    free(data);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(CORRUPT_V6_PATH, &status);
    assert(loaded == NULL);
    assert(status == ATP_ERR_FORMAT);
    remove(CORRUPT_V6_PATH);
}

/* Bitrot in a v6 image is rejected by the digest before decoding. */
static void test_bitrot_rejected(void) {
    const atp_neural_architecture architecture = one_hidden(7u);
    atp_graph *graph = graph_at(&architecture);
    assert(atp_graph_save(graph, CORRUPT_V6_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(CORRUPT_V6_PATH, &size);
    data[size / 2u] ^= 0x01u;
    write_file(CORRUPT_V6_PATH, data, size);
    free(data);

    atp_status status = ATP_OK;
    atp_graph *loaded = atp_graph_load(CORRUPT_V6_PATH, &status);
    assert(loaded == NULL);
    assert(status == ATP_ERR_FORMAT);
    remove(CORRUPT_V6_PATH);
}

/* Invalid architecture descriptors fail closed at creation: NULL graph,
 * never a partial one. */
static void test_invalid_architectures_rejected(void) {
    atp_graph_config config = atp_graph_default_config();

    assert(atp_graph_create_with_architecture(&config, NULL) == NULL);

    atp_neural_architecture architecture = three_hidden();
    atp_neural_architecture invalid = architecture;
    invalid.version = 99u;
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.embedding_dim = 0u;
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.input_dim = invalid.embedding_dim; /* not embedding_dim * 2 */
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.hidden_layer_count = 0u;
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.hidden_layer_count = ATPERSON_NEURAL_MAX_HIDDEN_LAYERS + 1u;
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.hidden_widths[2] = 0u; /* inactive-but-zero inner width */
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);

    invalid = architecture;
    invalid.output_dim = 2u; /* scalar output only */
    assert(atp_graph_create_with_architecture(&config, &invalid) == NULL);
}

/* Determinism: the same observations at the same architecture and seed
 * produce byte-identical v6 snapshots across independent runs. */
static void test_deterministic_save(void) {
    const atp_neural_architecture architecture = three_hidden();

    atp_graph *first = graph_at(&architecture);
    assert(atp_graph_save(first, V6_PATH) == ATP_OK);
    atp_graph_destroy(first);
    size_t first_size = 0u;
    unsigned char *first_data = read_file(V6_PATH, &first_size);

    atp_graph *second = graph_at(&architecture);
    assert(atp_graph_save(second, V6_PATH) == ATP_OK);
    atp_graph_destroy(second);
    size_t second_size = 0u;
    unsigned char *second_data = read_file(V6_PATH, &second_size);

    assert(first_size == second_size);
    assert(memcmp(first_data, second_data, first_size) == 0);
    free(first_data);
    free(second_data);
    remove(V6_PATH);
}

/* The architecture metadata probe (issue #73) reads the persisted topology
 * without materializing the graph. */
static void test_arch_probe(void) {
    const atp_neural_architecture architecture = three_hidden();
    atp_graph *graph = graph_at(&architecture);
    assert(atp_graph_save(graph, V6_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    atp_neural_architecture probed = {0};
    assert(atp_snapshot_neural_architecture(V6_PATH, &probed) == ATP_OK);
    assert(probed.version == architecture.version);
    assert(probed.embedding_dim == architecture.embedding_dim);
    assert(probed.input_dim == architecture.input_dim);
    assert(probed.output_dim == architecture.output_dim);
    assert(probed.hidden_layer_count == architecture.hidden_layer_count);
    for (uint32_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        assert(probed.hidden_widths[i] == architecture.hidden_widths[i]);
    }

    /* The probe never allocates the graph: a snapshot with no learned state
     * still reports its topology. */
    assert(atp_snapshot_neural_architecture(V6_PATH, NULL) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_snapshot_neural_architecture(NULL, &probed) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_snapshot_neural_architecture("", &probed) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_snapshot_neural_architecture(
               "atperson-snapshot-test-does-not-exist.bin", &probed) == ATP_ERR_IO);

    remove(V6_PATH);
}

/* v5 snapshots carry no descriptor; the probe reports the legacy topology,
 * matching how they load. */
static void test_arch_probe_legacy(void) {
    atp_graph_config config = atp_graph_default_config();
    atp_graph *graph = atp_graph_create(&config);
    assert(graph != NULL);
    assert(atp_graph_save(graph, V5_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    atp_neural_architecture probed = {0};
    assert(atp_snapshot_neural_architecture(V5_PATH, &probed) == ATP_OK);
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert(probed.version == legacy.version);
    assert(probed.embedding_dim == legacy.embedding_dim);
    assert(probed.input_dim == legacy.input_dim);
    assert(probed.output_dim == legacy.output_dim);
    assert(probed.hidden_layer_count == legacy.hidden_layer_count);
    for (uint32_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        assert(probed.hidden_widths[i] == legacy.hidden_widths[i]);
    }
    remove(V5_PATH);
}

/* Legacy probes must validate the legacy snapshot version and minimum framing,
 * not accept a magic/version stub as a generation. */
static void test_arch_probe_rejects_invalid_legacy_framing(void) {
    unsigned char image[32] = {0};
    memcpy(image, "ATPERSN5", 8u);
    atp_store_u32le(image + 8u, UINT32_C(0xFFFFFFFF));
    write_file(V5_PATH, image, sizeof(image));

    atp_neural_architecture probed = {0};
    assert(atp_snapshot_neural_architecture(V5_PATH, &probed) == ATP_ERR_FORMAT);

    atp_store_u32le(image + 8u, 5u);
    write_file(V5_PATH, image, 12u);
    assert(atp_snapshot_neural_architecture(V5_PATH, &probed) == ATP_ERR_FORMAT);
    remove(V5_PATH);
}

/* A structurally invalid ARCH payload in an otherwise intact v6 snapshot is
 * FORMAT from the probe as well as from the whole-file loader. */
static void test_arch_probe_rejects_corrupt(void) {
    const atp_neural_architecture architecture = three_hidden();
    atp_graph *graph = graph_at(&architecture);
    assert(atp_graph_save(graph, CORRUPT_V6_PATH) == ATP_OK);
    atp_graph_destroy(graph);

    size_t size = 0u;
    unsigned char *data = read_file(CORRUPT_V6_PATH, &size);
    size_t position = 12u;
    int found = 0;
    while (position + 12u <= size - 8u) {
        const uint32_t tag = (uint32_t)data[position] | ((uint32_t)data[position + 1u] << 8u) |
                              ((uint32_t)data[position + 2u] << 16u) |
                              ((uint32_t)data[position + 3u] << 24u);
        uint64_t length = 0u;
        for (unsigned i = 0u; i < 8u; ++i) {
            length |= (uint64_t)data[position + 4u + i] << (8u * i);
        }
        if (tag == 11u) {
            /* hidden_layer_count is at payload offset 16. Force it out of
             * range so layout validation fails. */
            data[position + 12u + 16u] = 0xFFu;
            found = 1;
            break;
        }
        position += 12u + (size_t)length;
    }
    assert(found == 1);
    const uint64_t digest = atp_fnv1a64(data, size - 8u);
    for (unsigned i = 0u; i < 8u; ++i) {
        data[size - 8u + i] = (unsigned char)(digest >> (8u * i));
    }
    write_file(CORRUPT_V6_PATH, data, size);
    free(data);

    atp_neural_architecture probed = {0};
    assert(atp_snapshot_neural_architecture(CORRUPT_V6_PATH, &probed) == ATP_ERR_FORMAT);
    remove(CORRUPT_V6_PATH);
}

int main(void) {
    test_roundtrip_one_hidden();
    test_roundtrip_two_hidden();
    test_roundtrip_three_hidden();
    test_legacy_saves_v5();
    test_corrupt_arch_rejected();
    test_bitrot_rejected();
    test_invalid_architectures_rejected();
    test_deterministic_save();
    test_arch_probe();
    test_arch_probe_legacy();
    test_arch_probe_rejects_invalid_legacy_framing();
    test_arch_probe_rejects_corrupt();
    printf("v6_topology_test: all tests passed\n");
    return 0;
}
