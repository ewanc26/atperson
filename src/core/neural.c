#include "internal.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static float atp_sigmoid(float value) {
    if (value >= 0.0f) {
        const float z = expf(-value);
        return 1.0f / (1.0f + z);
    }
    const float z = expf(value);
    return z / (1.0f + z);
}

static void atp_pair_input(const atp_graph *graph, uint32_t source, uint32_t target,
                           float *input) {
    const size_t embedding_dim = graph->neural_architecture.embedding_dim;
    for (size_t i = 0u; i < embedding_dim; ++i) {
        input[i] = graph->nodes[source].embedding[i];
        input[embedding_dim + i] = graph->nodes[target].embedding[i];
    }
}

static float atp_forward(const atp_graph *graph, float *activations) {
    const atp_network *network = &graph->network;
    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        const float *input = activations + network->layout.activation_offsets[layer];
        float *output = activations + network->layout.activation_offsets[layer + 1u];
        const bool final_layer = layer + 1u == network->layout.layer_count;

        for (size_t o = 0u; o < output_width; ++o) {
            float activation = network->biases[atp_network_bias_index(network, layer, o)];
            for (size_t i = 0u; i < input_width; ++i) {
                activation +=
                    network->weights[atp_network_weight_index(network, layer, o, i)] * input[i];
            }
            output[o] = final_layer ? atp_sigmoid(activation) : tanhf(activation);
        }
    }

    return activations[network->layout.activation_offsets[network->layout.layer_count]];
}

static void atp_initialize_weights(atp_graph *graph, atp_network *network) {
    const float input_scale = 0.12f;
    const float output_scale = 0.12f;

    /*
     * Legacy initialization order is durable learning behaviour. Preserve the
     * historical draw sequence exactly: one hidden-to-output weight, followed
     * by that hidden unit's complete input row, repeated for every hidden unit.
     */
    if (atp_neural_architecture_is_legacy(&graph->neural_architecture)) {
        const size_t output_layer = network->layout.layer_count - 1u;
        const size_t hidden_width = network->layout.output_widths[0];
        const size_t input_width = network->layout.input_widths[0];
        for (size_t h = 0u; h < hidden_width; ++h) {
            network->weights[atp_network_weight_index(network, output_layer, 0u, h)] =
                atp_rng_signed(graph) * output_scale;
            for (size_t i = 0u; i < input_width; ++i) {
                network->weights[atp_network_weight_index(network, 0u, h, i)] =
                    atp_rng_signed(graph) * input_scale;
            }
        }
        return;
    }

    /*
     * Future variable architectures use deterministic row-major
     * initialization. The topology/algorithm version is persisted before
     * public construction of these shapes is enabled (#72).
     */
    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        for (size_t o = 0u; o < output_width; ++o) {
            for (size_t i = 0u; i < input_width; ++i) {
                network->weights[atp_network_weight_index(network, layer, o, i)] =
                    atp_rng_signed(graph) * input_scale;
            }
        }
    }
}

bool atp_network_init(atp_graph *graph) {
    if (!graph) {
        return false;
    }

    atp_neural_layout layout = {0};
    if (!atp_neural_layout_build(&graph->neural_architecture, &layout)) {
        return false;
    }

    atp_network network = {.layout = layout};
    network.weights = malloc(layout.weight_count * sizeof(*network.weights));
    network.biases = calloc(layout.bias_count, sizeof(*network.biases));
    network.weight_importance = calloc(layout.weight_count, sizeof(*network.weight_importance));
    network.bias_importance = calloc(layout.bias_count, sizeof(*network.bias_importance));
    network.training_activations =
        malloc(layout.activation_count * sizeof(*network.training_activations));
    network.training_deltas =
        malloc(layout.activation_count * sizeof(*network.training_deltas));

    if (!network.weights || !network.biases || !network.weight_importance ||
        !network.bias_importance || !network.training_activations || !network.training_deltas) {
        atp_network_destroy(&network);
        return false;
    }

    atp_initialize_weights(graph, &network);
    graph->network = network;
    return true;
}

void atp_network_destroy(atp_network *network) {
    if (!network) {
        return;
    }
    free(network->weights);
    free(network->biases);
    free(network->weight_importance);
    free(network->bias_importance);
    free(network->training_activations);
    free(network->training_deltas);
    memset(network, 0, sizeof(*network));
}

atp_status atp_network_score(const atp_graph *graph, uint32_t source, uint32_t target,
                             float *out_score) {
    if (!graph || !out_score || source >= graph->node_count || target >= graph->node_count) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    const size_t count = graph->network.layout.activation_count;
    float *activations = malloc(count * sizeof(*activations));
    if (!activations) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    atp_pair_input(graph, source, target, activations);
    *out_score = atp_forward(graph, activations);
    free(activations);
    return ATP_OK;
}

float atp_network_score_owned(atp_graph *graph, uint32_t source, uint32_t target) {
    float *activations = graph->network.training_activations;
    atp_pair_input(graph, source, target, activations);
    return atp_forward(graph, activations);
}

static void atp_apply_gradient(atp_graph *graph, float *parameter, float *importance,
                               float gradient, bool plasticity, float threshold, float scale,
                               bool *step_was_protected) {
    float rate = graph->config.learning_rate;
    if (plasticity && *importance > threshold) {
        rate *= scale;
        graph->plasticity_parameters_protected++;
        *step_was_protected = true;
    }
    *parameter -= rate * gradient;
    if (plasticity) {
        *importance += fabsf(gradient);
    }
}

float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target, float expected) {
    atp_network *network = &graph->network;
    float *activations = network->training_activations;
    float *deltas = network->training_deltas;
    memset(deltas, 0, network->layout.activation_count * sizeof(*deltas));

    atp_pair_input(graph, source, target, activations);
    const float output = atp_forward(graph, activations);

    const float epsilon = 1.0e-6f;
    const float clipped =
        output < epsilon ? epsilon : (output > 1.0f - epsilon ? 1.0f - epsilon : output);
    const float loss =
        -(expected * logf(clipped) + (1.0f - expected) * logf(1.0f - clipped));

    /* BCE + sigmoid: dL/dz for the scalar output is output - target. */
    const size_t final_activation =
        network->layout.activation_offsets[network->layout.layer_count];
    deltas[final_activation] = output - expected;

    /*
     * Back-propagate through every hidden layer before mutating any weight.
     * activation_offsets[layer] is the input activation vector for dense
     * layer N; layer 0's input is the raw embedding pair and therefore does
     * not receive a tanh derivative.
     */
    for (size_t layer = network->layout.layer_count - 1u; layer > 0u; --layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        const size_t input_offset = network->layout.activation_offsets[layer];
        const size_t output_offset = network->layout.activation_offsets[layer + 1u];

        for (size_t i = 0u; i < input_width; ++i) {
            float gradient = 0.0f;
            for (size_t o = 0u; o < output_width; ++o) {
                gradient += deltas[output_offset + o] *
                            network->weights[atp_network_weight_index(network, layer, o, i)];
            }
            const float activation = activations[input_offset + i];
            deltas[input_offset + i] = gradient * (1.0f - activation * activation);
        }
    }

    /* Gradient with respect to the raw concatenated embedding input. */
    {
        const size_t input_width = network->layout.input_widths[0];
        const size_t output_width = network->layout.output_widths[0];
        const size_t output_offset = network->layout.activation_offsets[1u];
        for (size_t i = 0u; i < input_width; ++i) {
            float gradient = 0.0f;
            for (size_t o = 0u; o < output_width; ++o) {
                gradient += deltas[output_offset + o] *
                            network->weights[atp_network_weight_index(network, 0u, o, i)];
            }
            deltas[i] = gradient;
        }
    }

    const bool plasticity = graph->config.enable_plasticity_control;
    const float threshold = graph->config.plasticity_threshold > 0.0f
                                ? graph->config.plasticity_threshold
                                : 0.5f;
    const float scale = graph->config.plasticity_scale >= 0.0f
                            ? graph->config.plasticity_scale
                            : 0.1f;
    bool step_was_protected = false;
    if (plasticity) {
        graph->plasticity_steps_total++;
    }

    for (size_t layer = 0u; layer < network->layout.layer_count; ++layer) {
        const size_t input_width = network->layout.input_widths[layer];
        const size_t output_width = network->layout.output_widths[layer];
        const size_t input_offset = network->layout.activation_offsets[layer];
        const size_t output_offset = network->layout.activation_offsets[layer + 1u];

        for (size_t o = 0u; o < output_width; ++o) {
            const float delta = deltas[output_offset + o];
            for (size_t i = 0u; i < input_width; ++i) {
                const size_t index = atp_network_weight_index(network, layer, o, i);
                const float gradient = delta * activations[input_offset + i];
                atp_apply_gradient(graph, &network->weights[index],
                                   &network->weight_importance[index], gradient, plasticity,
                                   threshold, scale, &step_was_protected);
            }

            const size_t bias = atp_network_bias_index(network, layer, o);
            atp_apply_gradient(graph, &network->biases[bias], &network->bias_importance[bias],
                               delta, plasticity, threshold, scale, &step_was_protected);
        }
    }

    const size_t embedding_dim = graph->neural_architecture.embedding_dim;
    for (size_t i = 0u; i < embedding_dim; ++i) {
        atp_apply_gradient(graph, &graph->nodes[source].embedding[i],
                           &graph->nodes[source].embedding_importance[i], deltas[i],
                           plasticity, threshold, scale, &step_was_protected);
        atp_apply_gradient(graph, &graph->nodes[target].embedding[i],
                           &graph->nodes[target].embedding_importance[i],
                           deltas[embedding_dim + i], plasticity, threshold, scale,
                           &step_was_protected);
    }

    if (plasticity && step_was_protected) {
        graph->plasticity_steps_protected++;
    }
    return loss;
}

atp_status atp_graph_plasticity_report(const atp_graph *graph, atp_plasticity_report *out_report) {
    if (!graph || !out_report) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    out_report->steps_total = graph->plasticity_steps_total;
    out_report->steps_protected = graph->plasticity_steps_protected;
    out_report->parameters_protected = graph->plasticity_parameters_protected;
    return ATP_OK;
}
