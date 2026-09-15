#include "internal.h"

#include <math.h>
#include <stddef.h>

static float atp_sigmoid(float value) {
    if (value >= 0.0f) {
        const float z = expf(-value);
        return 1.0f / (1.0f + z);
    }
    const float z = expf(value);
    return z / (1.0f + z);
}

static void atp_pair_input(const atp_graph *graph, uint32_t source,
                           uint32_t target,
                           float input[ATPERSON_INPUT_DIM]) {
    for (size_t i = 0; i < ATPERSON_EMBEDDING_DIM; ++i) {
        input[i] = graph->nodes[source].embedding[i];
        input[ATPERSON_EMBEDDING_DIM + i] =
            graph->nodes[target].embedding[i];
    }
}

static float atp_forward(const atp_graph *graph,
                         const float input[ATPERSON_INPUT_DIM],
                         float hidden[ATPERSON_HIDDEN_DIM]) {
    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        float activation = graph->network.hidden_bias[h];
        for (size_t i = 0; i < ATPERSON_INPUT_DIM; ++i) {
            activation += graph->network.input_hidden[h][i] * input[i];
        }
        hidden[h] = tanhf(activation);
    }

    float output = graph->network.output_bias;
    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        output += graph->network.hidden_output[h] * hidden[h];
    }
    return atp_sigmoid(output);
}

void atp_network_init(atp_graph *graph) {
    const float input_scale = 0.12f;
    const float output_scale = 0.12f;

    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        graph->network.hidden_bias[h] = 0.0f;
        graph->network.hidden_output[h] =
            atp_rng_signed(graph) * output_scale;
        for (size_t i = 0; i < ATPERSON_INPUT_DIM; ++i) {
            graph->network.input_hidden[h][i] =
                atp_rng_signed(graph) * input_scale;
        }
    }
    graph->network.output_bias = 0.0f;
}

float atp_network_score(const atp_graph *graph, uint32_t source,
                        uint32_t target) {
    float input[ATPERSON_INPUT_DIM];
    float hidden[ATPERSON_HIDDEN_DIM];
    atp_pair_input(graph, source, target, input);
    return atp_forward(graph, input, hidden);
}

float atp_network_train(atp_graph *graph, uint32_t source, uint32_t target,
                        float expected) {
    float input[ATPERSON_INPUT_DIM];
    float hidden[ATPERSON_HIDDEN_DIM];
    float hidden_gradient[ATPERSON_HIDDEN_DIM];
    float input_gradient[ATPERSON_INPUT_DIM] = {0};

    atp_pair_input(graph, source, target, input);
    const float output = atp_forward(graph, input, hidden);

    const float epsilon = 1.0e-6f;
    const float clipped =
        output < epsilon ? epsilon : (output > 1.0f - epsilon
                                          ? 1.0f - epsilon
                                          : output);
    const float loss =
        -(expected * logf(clipped) + (1.0f - expected) * logf(1.0f - clipped));

    // Binary-cross-entropy + sigmoid derivative simplifies to output-target.
    const float output_gradient = output - expected;
    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        hidden_gradient[h] =
            output_gradient * graph->network.hidden_output[h] *
            (1.0f - hidden[h] * hidden[h]);
    }

    // Compute the embedding gradient before mutating the matrix it depends on.
    for (size_t i = 0; i < ATPERSON_INPUT_DIM; ++i) {
        float gradient = 0.0f;
        for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
            gradient += hidden_gradient[h] *
                        graph->network.input_hidden[h][i];
        }
        input_gradient[i] = gradient;
    }

    const float rate = graph->config.learning_rate;
    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        graph->network.hidden_output[h] -=
            rate * output_gradient * hidden[h];
        graph->network.hidden_bias[h] -= rate * hidden_gradient[h];
        for (size_t i = 0; i < ATPERSON_INPUT_DIM; ++i) {
            graph->network.input_hidden[h][i] -=
                rate * hidden_gradient[h] * input[i];
        }
    }
    graph->network.output_bias -= rate * output_gradient;

    for (size_t i = 0; i < ATPERSON_EMBEDDING_DIM; ++i) {
        graph->nodes[source].embedding[i] -= rate * input_gradient[i];
        graph->nodes[target].embedding[i] -=
            rate * input_gradient[ATPERSON_EMBEDDING_DIM + i];
    }

    return loss;
}
