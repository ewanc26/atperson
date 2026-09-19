#include "internal.h"

#include <string.h>

bool atp_neural_architecture_is_legacy(const atp_neural_architecture *architecture) {
    if (!architecture) {
        return false;
    }
    const atp_neural_architecture legacy = atp_neural_legacy_architecture();
    return architecture->version == legacy.version &&
           architecture->embedding_dim == legacy.embedding_dim &&
           architecture->input_dim == legacy.input_dim &&
           architecture->hidden_layer_count == legacy.hidden_layer_count &&
           architecture->output_dim == legacy.output_dim &&
           memcmp(architecture->hidden_widths, legacy.hidden_widths,
                  sizeof(legacy.hidden_widths)) == 0;
}

bool atp_neural_layout_build(const atp_neural_architecture *architecture,
                             atp_neural_layout *out_layout) {
    if (!architecture || !out_layout ||
        architecture->version != ATPERSON_NEURAL_ARCHITECTURE_VERSION ||
        architecture->embedding_dim == 0u ||
        architecture->embedding_dim > ATPERSON_NEURAL_WIDTH_LIMIT ||
        architecture->hidden_layer_count == 0u ||
        architecture->hidden_layer_count > ATPERSON_NEURAL_MAX_HIDDEN_LAYERS ||
        architecture->output_dim != 1u) {
        return false;
    }

    const uint64_t expected_input = (uint64_t)architecture->embedding_dim * 2u;
    if (expected_input > UINT32_MAX || architecture->input_dim != (uint32_t)expected_input) {
        return false;
    }

    for (uint32_t layer = 0u; layer < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++layer) {
        const uint32_t width = architecture->hidden_widths[layer];
        if (layer < architecture->hidden_layer_count) {
            if (width == 0u || width > ATPERSON_NEURAL_WIDTH_LIMIT) {
                return false;
            }
        } else if (width != 0u) {
            return false;
        }
    }

    atp_neural_layout layout = {0};
    layout.layer_count = (size_t)architecture->hidden_layer_count + 1u;
    layout.activation_offsets[0] = 0u;
    layout.activation_count = architecture->input_dim;

    size_t previous = architecture->input_dim;
    for (size_t layer = 0u; layer < layout.layer_count; ++layer) {
        const size_t output =
            layer < architecture->hidden_layer_count
                ? architecture->hidden_widths[layer]
                : architecture->output_dim;

        if (previous == 0u || output == 0u || previous > SIZE_MAX / output) {
            return false;
        }
        const size_t layer_weights = previous * output;
        if (layer_weights > SIZE_MAX - layout.weight_count ||
            output > SIZE_MAX - layout.bias_count ||
            output > SIZE_MAX - layout.activation_count) {
            return false;
        }

        layout.input_widths[layer] = previous;
        layout.output_widths[layer] = output;
        layout.weight_offsets[layer] = layout.weight_count;
        layout.bias_offsets[layer] = layout.bias_count;
        layout.activation_offsets[layer + 1u] = layout.activation_count;

        layout.weight_count += layer_weights;
        layout.bias_count += output;
        layout.activation_count += output;
        previous = output;
    }

    if (layout.weight_count > ATPERSON_NEURAL_PARAMETER_LIMIT ||
        layout.bias_count > ATPERSON_NEURAL_PARAMETER_LIMIT - layout.weight_count ||
        layout.weight_count > SIZE_MAX / sizeof(float) ||
        layout.bias_count > SIZE_MAX / sizeof(float) ||
        layout.activation_count > SIZE_MAX / sizeof(float)) {
        return false;
    }

    *out_layout = layout;
    return true;
}

uint64_t atp_neural_parameter_count(const atp_neural_architecture *architecture) {
    if (!architecture) {
        return 0u;
    }
    atp_neural_layout layout = {0};
    if (!atp_neural_layout_build(architecture, &layout)) {
        return 0u;
    }
    return (uint64_t)layout.weight_count + (uint64_t)layout.bias_count;
}
