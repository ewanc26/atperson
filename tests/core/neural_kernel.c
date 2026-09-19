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

static atp_neural_architecture architecture_expanded(void) {
    return (atp_neural_architecture){
        .version = ATPERSON_NEURAL_ARCHITECTURE_VERSION,
        .embedding_dim = 6u,
        .input_dim = 12u,
        .hidden_layer_count = 3u,
        .hidden_widths = {8u, 5u, 4u, 0u},
        .output_dim = 1u,
    };
}

static atp_neural_architecture architecture_expanded_again(void) {
    return (atp_neural_architecture){
        .version = ATPERSON_NEURAL_ARCHITECTURE_VERSION,
        .embedding_dim = 8u,
        .input_dim = 16u,
        .hidden_layer_count = 3u,
        .hidden_widths = {10u, 7u, 5u, 0u},
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

static void fill_unique_learned_state(atp_graph *graph) {
    for (size_t i = 0u; i < graph->network.layout.weight_count; ++i) {
        graph->network.weights[i] = (float)(1000u + i);
        graph->network.weight_importance[i] = (float)(2000u + i);
    }
    for (size_t i = 0u; i < graph->network.layout.bias_count; ++i) {
        graph->network.biases[i] = (float)(3000u + i);
        graph->network.bias_importance[i] = (float)(4000u + i);
    }
    for (size_t node = 0u; node < graph->node_count; ++node) {
        for (size_t d = 0u; d < graph->neural_architecture.embedding_dim; ++d) {
            graph->nodes[node].embedding[d] =
                (float)(5000u + node * 100u + d);
            graph->nodes[node].embedding_importance[d] =
                (float)(6000u + node * 100u + d);
        }
    }
}

static void assert_network_equal(const atp_graph *left, const atp_graph *right) {
    assert(left->network.layout.weight_count == right->network.layout.weight_count);
    assert(left->network.layout.bias_count == right->network.layout.bias_count);
    assert(memcmp(left->network.weights, right->network.weights,
                  left->network.layout.weight_count * sizeof(float)) == 0);
    assert(memcmp(left->network.biases, right->network.biases,
                  left->network.layout.bias_count * sizeof(float)) == 0);
    assert(memcmp(left->network.weight_importance, right->network.weight_importance,
                  left->network.layout.weight_count * sizeof(float)) == 0);
    assert(memcmp(left->network.bias_importance, right->network.bias_importance,
                  left->network.layout.bias_count * sizeof(float)) == 0);
    assert(left->node_count == right->node_count);
    for (size_t node = 0u; node < left->node_count; ++node) {
        assert(memcmp(left->nodes[node].embedding, right->nodes[node].embedding,
                      left->neural_architecture.embedding_dim * sizeof(float)) == 0);
        assert(memcmp(left->nodes[node].embedding_importance,
                      right->nodes[node].embedding_importance,
                      left->neural_architecture.embedding_dim * sizeof(float)) == 0);
    }
}

static void test_migration_validation(void) {
    const atp_neural_architecture source = architecture_2_hidden();
    const atp_neural_architecture target = architecture_expanded();
    atp_neural_migration migration = {
        .version = ATPERSON_NEURAL_MIGRATION_VERSION,
        .seed = UINT64_C(0x1234abcd5678ef00),
        .source = source,
        .target = target,
    };
    assert(atp_neural_migration_validate(&migration) == ATP_OK);
    assert(atp_neural_migration_validate(NULL) == ATP_ERR_INVALID_ARGUMENT);

    atp_neural_migration invalid = migration;
    invalid.version++;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    invalid = migration;
    invalid.seed = 0u;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    invalid = migration;
    invalid.target = source;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    invalid = migration;
    invalid.target.embedding_dim = 3u;
    invalid.target.input_dim = 6u;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    invalid = migration;
    invalid.target.hidden_layer_count = 1u;
    invalid.target.hidden_widths[1] = 0u;
    invalid.target.hidden_widths[2] = 0u;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    invalid = migration;
    invalid.target.hidden_widths[1] = 2u;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);

    /* Appending a hidden layer must still leave the role-mapped scalar output
     * layer wide enough to preserve every old output weight. */
    invalid = migration;
    invalid.target.hidden_widths[2] = 2u;
    assert(atp_neural_migration_validate(&invalid) == ATP_ERR_MIGRATION);
}

static void assert_first_expansion_preserved(const atp_graph *graph) {
    const atp_neural_architecture source = architecture_2_hidden();
    atp_graph source_shape = {0};
    source_shape.neural_architecture = source;
    source_shape.rng_state = UINT64_C(1);
    assert(atp_network_init(&source_shape));
    for (size_t i = 0u; i < source_shape.network.layout.weight_count; ++i) {
        source_shape.network.weights[i] = (float)(1000u + i);
        source_shape.network.weight_importance[i] = (float)(2000u + i);
    }
    for (size_t i = 0u; i < source_shape.network.layout.bias_count; ++i) {
        source_shape.network.biases[i] = (float)(3000u + i);
        source_shape.network.bias_importance[i] = (float)(4000u + i);
    }

    /* Hidden layer 0: source and target embedding halves move independently. */
    for (size_t output = 0u; output < source.hidden_widths[0]; ++output) {
        for (size_t d = 0u; d < source.embedding_dim; ++d) {
            const size_t old_source =
                atp_network_weight_index(&source_shape.network, 0u, output, d);
            const size_t new_source =
                atp_network_weight_index(&graph->network, 0u, output, d);
            assert(graph->network.weights[new_source] ==
                   source_shape.network.weights[old_source]);
            assert(graph->network.weight_importance[new_source] ==
                   source_shape.network.weight_importance[old_source]);

            const size_t old_target =
                atp_network_weight_index(&source_shape.network, 0u, output,
                                         source.embedding_dim + d);
            const size_t new_target =
                atp_network_weight_index(&graph->network, 0u, output,
                                         graph->neural_architecture.embedding_dim + d);
            assert(graph->network.weights[new_target] ==
                   source_shape.network.weights[old_target]);
            assert(graph->network.weight_importance[new_target] ==
                   source_shape.network.weight_importance[old_target]);
        }
        const size_t old_bias =
            atp_network_bias_index(&source_shape.network, 0u, output);
        const size_t new_bias =
            atp_network_bias_index(&graph->network, 0u, output);
        assert(graph->network.biases[new_bias] ==
               source_shape.network.biases[old_bias]);
        assert(graph->network.bias_importance[new_bias] ==
               source_shape.network.bias_importance[old_bias]);
    }

    /* Existing hidden layer 1 keeps its overlapping rectangle and bias. */
    for (size_t output = 0u; output < source.hidden_widths[1]; ++output) {
        for (size_t input = 0u; input < source.hidden_widths[0]; ++input) {
            const size_t old_weight =
                atp_network_weight_index(&source_shape.network, 1u, output, input);
            const size_t new_weight =
                atp_network_weight_index(&graph->network, 1u, output, input);
            assert(graph->network.weights[new_weight] ==
                   source_shape.network.weights[old_weight]);
            assert(graph->network.weight_importance[new_weight] ==
                   source_shape.network.weight_importance[old_weight]);
        }
    }

    /* The old scalar output layer moves from dense layer 2 to layer 3. */
    const size_t old_output_layer = source.hidden_layer_count;
    const size_t new_output_layer = graph->neural_architecture.hidden_layer_count;
    for (size_t input = 0u; input < source.hidden_widths[1]; ++input) {
        const size_t old_weight =
            atp_network_weight_index(&source_shape.network, old_output_layer, 0u, input);
        const size_t new_weight =
            atp_network_weight_index(&graph->network, new_output_layer, 0u, input);
        assert(graph->network.weights[new_weight] ==
               source_shape.network.weights[old_weight]);
        assert(graph->network.weight_importance[new_weight] ==
               source_shape.network.weight_importance[old_weight]);
    }
    const size_t old_output_bias =
        atp_network_bias_index(&source_shape.network, old_output_layer, 0u);
    const size_t new_output_bias =
        atp_network_bias_index(&graph->network, new_output_layer, 0u);
    assert(graph->network.biases[new_output_bias] ==
           source_shape.network.biases[old_output_bias]);
    assert(graph->network.bias_importance[new_output_bias] ==
           source_shape.network.bias_importance[old_output_bias]);

    for (size_t node = 0u; node < graph->node_count; ++node) {
        for (size_t d = 0u; d < source.embedding_dim; ++d) {
            assert(graph->nodes[node].embedding[d] ==
                   (float)(5000u + node * 100u + d));
            assert(graph->nodes[node].embedding_importance[d] ==
                   (float)(6000u + node * 100u + d));
        }
        for (size_t d = source.embedding_dim;
             d < graph->neural_architecture.embedding_dim; ++d) {
            assert(graph->nodes[node].embedding_importance[d] == 0.0f);
        }
    }

    atp_network_destroy(&source_shape.network);
}

static void test_deterministic_atomic_expansion(void) {
    const atp_neural_architecture source = architecture_2_hidden();
    const atp_neural_architecture target = architecture_expanded();
    atp_graph_config config = atp_graph_default_config();
    config.seed = UINT64_C(0xcafebabef00d1234);

    atp_graph *first = atp_graph_create_with_architecture(&config, &source);
    atp_graph *second = atp_graph_create_with_architecture(&config, &source);
    assert(first != NULL && second != NULL);
    assert(atp_graph_observe_text(first, "alpha beta gamma", "at://migration/1") == ATP_OK);
    assert(atp_graph_observe_text(second, "alpha beta gamma", "at://migration/1") == ATP_OK);
    fill_unique_learned_state(first);
    fill_unique_learned_state(second);

    const uint64_t first_rng = first->rng_state;
    const uint64_t second_rng = second->rng_state;
    atp_neural_migration migration = {
        .version = ATPERSON_NEURAL_MIGRATION_VERSION,
        .seed = UINT64_C(0x1122334455667788),
        .source = source,
        .target = target,
    };
    assert(atp_graph_expand_neural(first, &migration) == ATP_OK);
    assert(atp_graph_expand_neural(second, &migration) == ATP_OK);
    assert(first->rng_state == first_rng);
    assert(second->rng_state == second_rng);
    assert(memcmp(&first->neural_architecture, &target, sizeof(target)) == 0);
    assert(memcmp(&second->neural_architecture, &target, sizeof(target)) == 0);
    assert_network_equal(first, second);
    assert_first_expansion_preserved(first);

    /* New coordinates are deterministic learned-state initialisation, not
     * silently all-zero padding. */
    bool saw_nonzero_new_embedding = false;
    for (size_t node = 0u; node < first->node_count; ++node) {
        for (size_t d = source.embedding_dim; d < target.embedding_dim; ++d) {
            if (first->nodes[node].embedding[d] != 0.0f) {
                saw_nonzero_new_embedding = true;
            }
        }
    }
    assert(saw_nonzero_new_embedding);

    /* A second monotonic expansion is supported and leaves the ordinary RNG
     * untouched again. */
    const atp_neural_architecture next = architecture_expanded_again();
    const uint64_t before_second_rng = first->rng_state;
    atp_neural_migration second_migration = {
        .version = ATPERSON_NEURAL_MIGRATION_VERSION,
        .seed = UINT64_C(0x8877665544332211),
        .source = target,
        .target = next,
    };
    assert(atp_graph_expand_neural(first, &second_migration) == ATP_OK);
    assert(first->rng_state == before_second_rng);
    assert(memcmp(&first->neural_architecture, &next, sizeof(next)) == 0);

    /* Wrong-generation and invalid migrations fail before the commit point. */
    float *embedding_before = first->nodes[0].embedding;
    float *weights_before = first->network.weights;
    const atp_neural_architecture architecture_before = first->neural_architecture;
    atp_neural_migration wrong_generation = second_migration;
    wrong_generation.source = source;
    wrong_generation.target = architecture_expanded_again();
    assert(atp_graph_expand_neural(first, &wrong_generation) == ATP_ERR_MIGRATION);
    assert(first->nodes[0].embedding == embedding_before);
    assert(first->network.weights == weights_before);
    assert(memcmp(&first->neural_architecture, &architecture_before,
                  sizeof(architecture_before)) == 0);

    atp_graph_destroy(first);
    atp_graph_destroy(second);
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
    test_migration_validation();
    test_deterministic_atomic_expansion();
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
