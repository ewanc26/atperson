#include "internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static atp_neural_architecture architecture_2_hidden(void) {
    return (atp_neural_architecture){
        .version = ATPERSON_NEURAL_ARCHITECTURE_VERSION,
        .embedding_dim = 4u,
        .input_dim = 8u,
        .hidden_layer_count = 2u,
        .hidden_widths = {6u, 3u, 0u, 0u},
        .output_dim = 1u,
    };
}

static atp_neural_architecture architecture_3_hidden(void) {
    return (atp_neural_architecture){
        .version = ATPERSON_NEURAL_ARCHITECTURE_VERSION,
        .embedding_dim = 4u,
        .input_dim = 8u,
        .hidden_layer_count = 3u,
        .hidden_widths = {8u, 6u, 4u, 0u},
        .output_dim = 1u,
    };
}

static void test_layouts(void) {
    atp_neural_layout layout = {0};
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert(atp_neural_layout_build(&legacy, &layout));
    assert(layout.layer_count == 2u);
    assert(layout.weight_count == 528u);
    assert(layout.bias_count == 17u);
    assert(layout.activation_count == 49u);

    const atp_neural_architecture two = architecture_2_hidden();
    assert(atp_neural_layout_build(&two, &layout));
    assert(layout.layer_count == 3u);
    assert(layout.weight_count == 69u);
    assert(layout.bias_count == 10u);
    assert(layout.activation_count == 18u);

    const atp_neural_architecture three = architecture_3_hidden();
    assert(atp_neural_layout_build(&three, &layout));
    assert(layout.layer_count == 4u);
    assert(layout.weight_count == 140u);
    assert(layout.bias_count == 19u);
    assert(layout.activation_count == 27u);

    atp_neural_architecture invalid = three;
    invalid.input_dim = 7u;
    assert(!atp_neural_layout_build(&invalid, &layout));
    invalid = three;
    invalid.hidden_widths[3] = 1u;
    assert(!atp_neural_layout_build(&invalid, &layout));
    invalid = three;
    invalid.output_dim = 2u;
    assert(!atp_neural_layout_build(&invalid, &layout));
}

static void exercise_kernel(const atp_neural_architecture *architecture) {
    atp_graph graph = {0};
    graph.config = atp_graph_default_config();
    graph.config.enable_plasticity_control = true;
    graph.config.plasticity_threshold = 1000.0f;
    graph.config.plasticity_scale = 0.1f;
    graph.rng_state = UINT64_C(0x123456789abcdef);
    graph.neural_architecture = *architecture;

    assert(atp_network_init(&graph));

    atp_node nodes[2] = {0};
    graph.nodes = nodes;
    graph.node_count = 2u;
    assert(atp_node_allocate_vectors(&graph, &nodes[0]));
    assert(atp_node_allocate_vectors(&graph, &nodes[1]));

    for (size_t i = 0u; i < architecture->embedding_dim; ++i) {
        nodes[0].embedding[i] = (float)(i + 1u) * 0.01f;
        nodes[1].embedding[i] = (float)(i + 1u) * -0.015f;
    }

    float before = 0.0f;
    assert(atp_network_score(&graph, 0u, 1u, &before) == ATP_OK);
    assert(isfinite(before) && before > 0.0f && before < 1.0f);

    for (unsigned step = 0u; step < 12u; ++step) {
        const float loss = atp_network_train(&graph, 0u, 1u, 1.0f);
        assert(isfinite(loss) && loss >= 0.0f);
    }

    float after = 0.0f;
    assert(atp_network_score(&graph, 0u, 1u, &after) == ATP_OK);
    assert(isfinite(after) && after > 0.0f && after < 1.0f);
    assert(after != before);

    atp_node_destroy(&nodes[0]);
    atp_node_destroy(&nodes[1]);
    atp_network_destroy(&graph.network);
}

int main(void) {
    test_layouts();

    const atp_neural_architecture two = architecture_2_hidden();
    const atp_neural_architecture three = architecture_3_hidden();
    exercise_kernel(&two);
    exercise_kernel(&three);

    puts("neural-kernel: ok");
    return 0;
}
