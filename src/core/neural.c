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

    for (size_t h = 0; h < ATPERSON_HIDDEN_DIM; ++h) {
        const float grad_out = output_gradient * hidden[h];
        float rate_out = rate;
        if (plasticity && graph->network.hidden_output_importance[h] > threshold) {
            rate_out *= scale;
            graph->plasticity_parameters_protected++;
            step_was_protected = true;
        }
        graph->network.hidden_output[h] -= rate_out * grad_out;
        if (plasticity) {
            graph->network.hidden_output_importance[h] += fabsf(grad_out);
        }

        const float grad_bias = hidden_gradient[h];
        float rate_bias = rate;
        if (plasticity && graph->network.hidden_bias_importance[h] > threshold) {
            rate_bias *= scale;
            graph->plasticity_parameters_protected++;
            step_was_protected = true;
        }
        graph->network.hidden_bias[h] -= rate_bias * grad_bias;
        if (plasticity) {
            graph->network.hidden_bias_importance[h] += fabsf(grad_bias);
        }

        for (size_t i = 0; i < ATPERSON_INPUT_DIM; ++i) {
            const float grad_in = hidden_gradient[h] * input[i];
            float rate_in = rate;
            if (plasticity && graph->network.input_hidden_importance[h][i] > threshold) {
                rate_in *= scale;
                graph->plasticity_parameters_protected++;
                step_was_protected = true;
            }
            graph->network.input_hidden[h][i] -= rate_in * grad_in;
            if (plasticity) {
                graph->network.input_hidden_importance[h][i] += fabsf(grad_in);
            }
        }
    }

    {
        const float grad_ob = output_gradient;
        float rate_ob = rate;
        if (plasticity && graph->network.output_bias_importance > threshold) {
            rate_ob *= scale;
            graph->plasticity_parameters_protected++;
            step_was_protected = true;
        }
        graph->network.output_bias -= rate_ob * grad_ob;
        if (plasticity) {
            graph->network.output_bias_importance += fabsf(grad_ob);
        }
    }

    for (size_t i = 0; i < ATPERSON_EMBEDDING_DIM; ++i) {
        const float grad_src = input_gradient[i];
        float rate_src = rate;
        if (plasticity && graph->nodes[source].embedding_importance[i] > threshold) {
            rate_src *= scale;
            graph->plasticity_parameters_protected++;
            step_was_protected = true;
        }
        graph->nodes[source].embedding[i] -= rate_src * grad_src;
        if (plasticity) {
            graph->nodes[source].embedding_importance[i] += fabsf(grad_src);
        }

        const float grad_tgt = input_gradient[ATPERSON_EMBEDDING_DIM + i];
        float rate_tgt = rate;
        if (plasticity && graph->nodes[target].embedding_importance[i] > threshold) {
            rate_tgt *= scale;
            graph->plasticity_parameters_protected++;
            step_was_protected = true;
        }
        graph->nodes[target].embedding[i] -= rate_tgt * grad_tgt;
        if (plasticity) {
            graph->nodes[target].embedding_importance[i] += fabsf(grad_tgt);
        }
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
