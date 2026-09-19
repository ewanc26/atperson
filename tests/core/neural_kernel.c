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

static void assert_layout_sane(const atp_neural_architecture *architecture,
                               const atp_neural_layout *layout, size_t expected_layers) {
    assert(layout->layer_count == expected_layers);
    assert(layout->weight_count <= ATPERSON_NEURAL_PARAMETER_LIMIT);
    assert(layout->bias_count <= ATPERSON_NEURAL_PARAMETER_LIMIT);
    assert(layout->activation_count >= architecture->input_dim);
    /* The public parameter count (issue #73) equals weights + biases. */
    assert(atp_neural_parameter_count(architecture) ==
           (uint64_t)layout->weight_count + (uint64_t)layout->bias_count);

    /* Dense layers are contiguous: the raw input block occupies [0, input_dim)
     * and each layer's output block starts immediately after the previous
     * one. Layer 0 reads the input block itself. */
    size_t expected_weights = 0u;
    size_t expected_biases = 0u;
    size_t expected_activations = architecture->input_dim;
    size_t previous = architecture->input_dim;
    assert(layout->activation_offsets[0] == 0u);
    for (size_t layer = 0u; layer < layout->layer_count; ++layer) {
        const size_t output =
            layer < architecture->hidden_layer_count
                ? architecture->hidden_widths[layer]
                : architecture->output_dim;
        assert(layout->input_widths[layer] == previous);
        assert(layout->output_widths[layer] == output);
        assert(layout->weight_offsets[layer] == expected_weights);
        assert(layout->bias_offsets[layer] == expected_biases);
        assert(layout->activation_offsets[layer + 1u] == expected_activations);

        expected_activations += output;
        expected_weights += previous * output;
        expected_biases += output;
        previous = output;
    }
    assert(layout->weight_count == expected_weights);
    assert(layout->bias_count == expected_biases);
    assert(layout->activation_count == expected_activations);
}

static void test_layouts(void) {
    atp_neural_layout layout = {0};
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    assert(atp_neural_layout_build(&legacy, &layout));
    assert_layout_sane(&legacy, &layout, 2u);
    assert(layout.weight_count == 528u);
    assert(layout.bias_count == 17u);
    assert(layout.activation_count == 49u);

    const atp_neural_architecture two = architecture_2_hidden();
    assert(atp_neural_layout_build(&two, &layout));
    assert_layout_sane(&two, &layout, 3u);
    assert(layout.weight_count == 69u);
    assert(layout.bias_count == 10u);
    assert(layout.activation_count == 18u);

    const atp_neural_architecture three = architecture_3_hidden();
    assert(atp_neural_layout_build(&three, &layout));
    assert_layout_sane(&three, &layout, 4u);
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
    invalid = three;
    invalid.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION + 1u;
    assert(!atp_neural_layout_build(&invalid, &layout));
    invalid = three;
    invalid.embedding_dim = 16384u;
    invalid.input_dim = 32768u;
    invalid.hidden_widths[0] = 16384u;
    assert(!atp_neural_layout_build(&invalid, &layout));
}

static void assert_network_indices_in_bounds(const atp_network *network) {
    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        for (size_t o = 0u; o < network->layout.output_widths[layer]; ++o) {
            assert(atp_network_bias_index(network, layer, o) < network->layout.bias_count);
            for (size_t i = 0u; i < network->layout.input_widths[layer]; ++i) {
                assert(atp_network_weight_index(network, layer, o, i) <
                       network->layout.weight_count);
            }
        }
    }
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
    assert_network_indices_in_bounds(&graph.network);

    for (unsigned step = 0u; step < 64u; ++step) {
        const float loss = atp_network_train(&graph, 0u, 1u, 1.0f);
        assert(isfinite(loss) && loss >= 0.0f);
    }

    float after = 0.0f;
    assert(atp_network_score(&graph, 0u, 1u, &after) == ATP_OK);
    assert(isfinite(after) && after > 0.0f && after < 1.0f);
    /* Gradient descent toward the target must move the output in that
     * direction, not merely change it. */
    assert(after > before);
    assert_network_indices_in_bounds(&graph.network);

    atp_node_destroy(&nodes[0]);
    atp_node_destroy(&nodes[1]);
    atp_network_destroy(&graph.network);
}

static void test_public_neural_score(void) {
    const atp_neural_architecture architecture = architecture_2_hidden();
    atp_graph_config config = atp_graph_default_config();
    config.seed = UINT64_C(0x123456789);
    atp_graph *graph = atp_graph_create_with_architecture(&config, &architecture);
    assert(graph != NULL);
    assert(atp_graph_observe_text(graph, "alpha beta", "at://test/neural-score") == ATP_OK);

    float score = 0.0f;
    assert(atp_graph_neural_score(graph, "alpha", "beta", &score) == ATP_OK);
    assert(isfinite(score) && score > 0.0f && score < 1.0f);
    assert(atp_graph_neural_score(graph, "alpha", "missing", &score) == ATP_ERR_NOT_FOUND);
    assert(atp_graph_neural_score(NULL, "alpha", "beta", &score) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_neural_score(graph, NULL, "beta", &score) == ATP_ERR_INVALID_ARGUMENT);
    assert(atp_graph_neural_score(graph, "alpha", "beta", NULL) == ATP_ERR_INVALID_ARGUMENT);

    atp_graph_destroy(graph);
}

static void test_deterministic_repeat(const atp_neural_architecture *architecture) {
    atp_graph first = {0};
    atp_graph second = {0};
    first.config = second.config = atp_graph_default_config();
    first.config.enable_plasticity_control = true;
    first.config.plasticity_threshold = 1000.0f;
    first.rng_state = second.rng_state = UINT64_C(0xabcdef1234567890);
    first.neural_architecture = second.neural_architecture = *architecture;

    assert(atp_network_init(&first));
    assert(atp_network_init(&second));
    assert(first.network.layout.weight_count == second.network.layout.weight_count);
    assert(first.network.layout.activation_count == second.network.layout.activation_count);

    atp_node first_nodes[2] = {0};
    atp_node second_nodes[2] = {0};
    first.nodes = first_nodes;
    first.node_count = 2u;
    second.nodes = second_nodes;
    second.node_count = 2u;
    for (size_t i = 0u; i < 2u; ++i) {
        assert(atp_node_allocate_vectors(&first, &first_nodes[i]));
        assert(atp_node_allocate_vectors(&second, &second_nodes[i]));
    }
    for (size_t i = 0u; i < architecture->embedding_dim; ++i) {
        const float value = (float)(i + 1u) * 0.02f;
        first_nodes[0].embedding[i] = value;
        first_nodes[1].embedding[i] = -value;
        second_nodes[0].embedding[i] = value;
        second_nodes[1].embedding[i] = -value;
    }

    for (unsigned step = 0u; step < 32u; ++step) {
        const float first_loss = atp_network_train(&first, 0u, 1u, 1.0f);
        const float second_loss = atp_network_train(&second, 0u, 1u, 1.0f);
        assert(first_loss == second_loss);
    }

    float first_score = 0.0f;
    float second_score = 0.0f;
    assert(atp_network_score(&first, 0u, 1u, &first_score) == ATP_OK);
    assert(atp_network_score(&second, 0u, 1u, &second_score) == ATP_OK);
    assert(first_score == second_score);

    for (size_t d = 0u; d < architecture->embedding_dim; ++d) {
        assert(first_nodes[0].embedding[d] == second_nodes[0].embedding[d]);
        assert(first_nodes[1].embedding[d] == second_nodes[1].embedding[d]);
    }

    atp_node_destroy(&first_nodes[0]);
    atp_node_destroy(&first_nodes[1]);
    atp_node_destroy(&second_nodes[0]);
    atp_node_destroy(&second_nodes[1]);
    atp_network_destroy(&first.network);
    atp_network_destroy(&second.network);
}

int main(void) {
    test_layouts();

    const atp_neural_architecture two = architecture_2_hidden();
    const atp_neural_architecture three = architecture_3_hidden();
    exercise_kernel(&two);
    exercise_kernel(&three);
    test_public_neural_score();
    test_deterministic_repeat(&two);
    test_deterministic_repeat(&three);

    /* Issue #73: invalid and NULL descriptors report a zero parameter count. */
    assert(atp_neural_parameter_count(NULL) == 0u);
    atp_neural_architecture invalid = three;
    invalid.hidden_layer_count = 0u;
    assert(atp_neural_parameter_count(&invalid) == 0u);

    puts("neural-kernel: ok");
    return 0;
}
