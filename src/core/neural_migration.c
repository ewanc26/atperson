#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct atp_migrated_node_vectors {
    float *embedding;
    float *importance;
} atp_migrated_node_vectors;

static bool atp_architecture_equal(const atp_neural_architecture *left,
                                   const atp_neural_architecture *right) {
    return left && right &&
           left->version == right->version &&
           left->embedding_dim == right->embedding_dim &&
           left->input_dim == right->input_dim &&
           left->hidden_layer_count == right->hidden_layer_count &&
           left->output_dim == right->output_dim &&
           memcmp(left->hidden_widths, right->hidden_widths,
                  sizeof(left->hidden_widths)) == 0;
}

static uint64_t atp_migration_rng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12u;
    x ^= x << 25u;
    x ^= x >> 27u;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static float atp_migration_rng_signed(uint64_t *state) {
    const uint64_t value = atp_migration_rng_next(state) >> 40u;
    const float unit = (float)value / (float)UINT32_C(16777215);
    return (unit * 2.0f) - 1.0f;
}

static uint64_t atp_migration_rng_state(const atp_neural_migration *migration) {
    uint64_t state = migration->seed ^ UINT64_C(0x4D49475241544531);
    state ^= (uint64_t)migration->version * UINT64_C(0x9E3779B97F4A7C15);
    if (state == 0u) {
        state = UINT64_C(0xA7C15D1CEB00B135);
    }
    return state;
}

static void atp_free_migrated_vectors(atp_migrated_node_vectors *vectors,
                                      size_t count) {
    if (!vectors) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        free(vectors[i].embedding);
        free(vectors[i].importance);
    }
    free(vectors);
}

atp_status atp_neural_migration_validate(const atp_neural_migration *migration) {
    if (!migration) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (migration->version != ATPERSON_NEURAL_MIGRATION_VERSION ||
        migration->seed == 0u) {
        return ATP_ERR_MIGRATION;
    }

    atp_neural_layout source_layout = {0};
    atp_neural_layout target_layout = {0};
    if (!atp_neural_layout_build(&migration->source, &source_layout) ||
        !atp_neural_layout_build(&migration->target, &target_layout)) {
        return ATP_ERR_MIGRATION;
    }

    const atp_neural_architecture *source = &migration->source;
    const atp_neural_architecture *target = &migration->target;
    if (source->version != target->version ||
        source->output_dim != target->output_dim ||
        target->embedding_dim < source->embedding_dim ||
        target->hidden_layer_count < source->hidden_layer_count) {
        return ATP_ERR_MIGRATION;
    }

    bool grew = target->embedding_dim > source->embedding_dim ||
                target->hidden_layer_count > source->hidden_layer_count;
    for (uint32_t layer = 0u; layer < source->hidden_layer_count; ++layer) {
        if (target->hidden_widths[layer] < source->hidden_widths[layer]) {
            return ATP_ERR_MIGRATION;
        }
        if (target->hidden_widths[layer] > source->hidden_widths[layer]) {
            grew = true;
        }
    }

    /*
     * The scalar output layer is mapped by role, not by dense-layer ordinal.
     * If new hidden layers are appended, its input becomes the target's final
     * hidden layer. That final width must still accommodate every old output
     * weight or the migration would silently discard learned state.
     */
    const uint32_t source_final_hidden =
        source->hidden_widths[source->hidden_layer_count - 1u];
    const uint32_t target_final_hidden =
        target->hidden_widths[target->hidden_layer_count - 1u];
    if (target_final_hidden < source_final_hidden) {
        return ATP_ERR_MIGRATION;
    }

    return grew ? ATP_OK : ATP_ERR_MIGRATION;
}

static void atp_copy_parameter(float *target_value, float *target_importance,
                               float source_value, float source_importance) {
    *target_value = source_value;
    *target_importance = source_importance;
}

static void atp_copy_hidden_layer(const atp_graph *graph,
                                  atp_network *target_network,
                                  const atp_neural_architecture *target_architecture,
                                  size_t layer) {
    const atp_network *source_network = &graph->network;
    const atp_neural_architecture *source_architecture =
        &graph->neural_architecture;
    const size_t output_width = source_architecture->hidden_widths[layer];

    for (size_t output = 0u; output < output_width; ++output) {
        const size_t source_bias =
            atp_network_bias_index(source_network, layer, output);
        const size_t target_bias =
            atp_network_bias_index(target_network, layer, output);
        atp_copy_parameter(
            &target_network->biases[target_bias],
            &target_network->bias_importance[target_bias],
            source_network->biases[source_bias],
            source_network->bias_importance[source_bias]);

        if (layer == 0u) {
            const size_t source_embedding =
                source_architecture->embedding_dim;
            const size_t target_embedding =
                target_architecture->embedding_dim;
            for (size_t dimension = 0u; dimension < source_embedding;
                 ++dimension) {
                const size_t source_source_weight =
                    atp_network_weight_index(source_network, layer, output,
                                             dimension);
                const size_t target_source_weight =
                    atp_network_weight_index(target_network, layer, output,
                                             dimension);
                atp_copy_parameter(
                    &target_network->weights[target_source_weight],
                    &target_network->weight_importance[target_source_weight],
                    source_network->weights[source_source_weight],
                    source_network->weight_importance[source_source_weight]);

                const size_t source_target_weight =
                    atp_network_weight_index(source_network, layer, output,
                                             source_embedding + dimension);
                const size_t target_target_weight =
                    atp_network_weight_index(target_network, layer, output,
                                             target_embedding + dimension);
                atp_copy_parameter(
                    &target_network->weights[target_target_weight],
                    &target_network->weight_importance[target_target_weight],
                    source_network->weights[source_target_weight],
                    source_network->weight_importance[source_target_weight]);
            }
            continue;
        }

        const size_t input_width =
            source_architecture->hidden_widths[layer - 1u];
        for (size_t input = 0u; input < input_width; ++input) {
            const size_t source_weight =
                atp_network_weight_index(source_network, layer, output, input);
            const size_t target_weight =
                atp_network_weight_index(target_network, layer, output, input);
            atp_copy_parameter(
                &target_network->weights[target_weight],
                &target_network->weight_importance[target_weight],
                source_network->weights[source_weight],
                source_network->weight_importance[source_weight]);
        }
    }
}

static void atp_copy_output_layer(const atp_graph *graph,
                                  atp_network *target_network,
                                  const atp_neural_architecture *target_architecture) {
    const atp_network *source_network = &graph->network;
    const atp_neural_architecture *source_architecture =
        &graph->neural_architecture;
    const size_t source_layer = source_architecture->hidden_layer_count;
    const size_t target_layer = target_architecture->hidden_layer_count;

    const size_t source_bias =
        atp_network_bias_index(source_network, source_layer, 0u);
    const size_t target_bias =
        atp_network_bias_index(target_network, target_layer, 0u);
    atp_copy_parameter(&target_network->biases[target_bias],
                       &target_network->bias_importance[target_bias],
                       source_network->biases[source_bias],
                       source_network->bias_importance[source_bias]);

    const size_t input_width =
        source_architecture->hidden_widths[
            source_architecture->hidden_layer_count - 1u];
    for (size_t input = 0u; input < input_width; ++input) {
        const size_t source_weight =
            atp_network_weight_index(source_network, source_layer, 0u, input);
        const size_t target_weight =
            atp_network_weight_index(target_network, target_layer, 0u, input);
        atp_copy_parameter(
            &target_network->weights[target_weight],
            &target_network->weight_importance[target_weight],
            source_network->weights[source_weight],
            source_network->weight_importance[source_weight]);
    }
}

atp_status atp_graph_expand_neural(atp_graph *graph,
                                   const atp_neural_migration *migration) {
    if (!graph || !migration) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    const atp_status validation = atp_neural_migration_validate(migration);
    if (validation != ATP_OK) {
        return validation;
    }
    if (!atp_architecture_equal(&graph->neural_architecture,
                                &migration->source)) {
        return ATP_ERR_MIGRATION;
    }
    if (graph->neural_migration_count != 0u &&
        migration->ledger_boundary_id <
            graph->neural_migrations[graph->neural_migration_count - 1u]
                .ledger_boundary_id) {
        return ATP_ERR_MIGRATION;
    }

    /*
     * Allocate every replacement before touching authoritative graph state.
     * atp_network_init provides the validated buffer shape; migration v1 then
     * overwrites every target weight with its own versioned PRNG stream so
     * ordinary model-initialisation changes cannot alter migration replay.
     */
    atp_graph scratch = {0};
    scratch.neural_architecture = migration->target;
    scratch.rng_state = UINT64_C(1);
    if (!atp_network_init(&scratch)) {
        return ATP_ERR_OUT_OF_MEMORY;
    }

    uint64_t migration_rng = atp_migration_rng_state(migration);
    for (size_t i = 0u; i < scratch.network.layout.weight_count; ++i) {
        scratch.network.weights[i] =
            atp_migration_rng_signed(&migration_rng) * 0.12f;
    }
    memset(scratch.network.biases, 0,
           scratch.network.layout.bias_count * sizeof(*scratch.network.biases));
    memset(scratch.network.weight_importance, 0,
           scratch.network.layout.weight_count *
               sizeof(*scratch.network.weight_importance));
    memset(scratch.network.bias_importance, 0,
           scratch.network.layout.bias_count *
               sizeof(*scratch.network.bias_importance));

    atp_migrated_node_vectors *vectors = NULL;
    if (graph->node_count != 0u) {
        if (graph->node_count >
            SIZE_MAX / sizeof(atp_migrated_node_vectors)) {
            atp_network_destroy(&scratch.network);
            return ATP_ERR_OUT_OF_MEMORY;
        }
        vectors = calloc(graph->node_count, sizeof(*vectors));
        if (!vectors) {
            atp_network_destroy(&scratch.network);
            return ATP_ERR_OUT_OF_MEMORY;
        }
    }

    const size_t source_embedding = migration->source.embedding_dim;
    const size_t target_embedding = migration->target.embedding_dim;
    for (size_t node = 0u; node < graph->node_count; ++node) {
        vectors[node].embedding =
            malloc(target_embedding * sizeof(*vectors[node].embedding));
        vectors[node].importance =
            calloc(target_embedding, sizeof(*vectors[node].importance));
        if (!vectors[node].embedding || !vectors[node].importance) {
            atp_free_migrated_vectors(vectors, graph->node_count);
            atp_network_destroy(&scratch.network);
            return ATP_ERR_OUT_OF_MEMORY;
        }

        memcpy(vectors[node].embedding, graph->nodes[node].embedding,
               source_embedding * sizeof(*vectors[node].embedding));
        memcpy(vectors[node].importance,
               graph->nodes[node].embedding_importance,
               source_embedding * sizeof(*vectors[node].importance));
        for (size_t dimension = source_embedding;
             dimension < target_embedding; ++dimension) {
            vectors[node].embedding[dimension] =
                atp_migration_rng_signed(&migration_rng) * 0.05f;
        }
    }

    for (size_t layer = 0u;
         layer < migration->source.hidden_layer_count; ++layer) {
        atp_copy_hidden_layer(graph, &scratch.network, &migration->target,
                              layer);
    }
    atp_copy_output_layer(graph, &scratch.network, &migration->target);

    if (graph->neural_migration_count == SIZE_MAX ||
        graph->neural_migration_count + 1u >
            SIZE_MAX / sizeof(*graph->neural_migrations)) {
        atp_free_migrated_vectors(vectors, graph->node_count);
        atp_network_destroy(&scratch.network);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    atp_neural_migration *new_history =
        malloc((graph->neural_migration_count + 1u) * sizeof(*new_history));
    if (!new_history) {
        atp_free_migrated_vectors(vectors, graph->node_count);
        atp_network_destroy(&scratch.network);
        return ATP_ERR_OUT_OF_MEMORY;
    }
    if (graph->neural_migration_count != 0u) {
        memcpy(new_history, graph->neural_migrations,
               graph->neural_migration_count * sizeof(*new_history));
    }
    new_history[graph->neural_migration_count] = *migration;

    /*
     * Commit point. There are no fallible operations below this line.
     * Swap authoritative pointers/topology, then release the old storage.
     */
    atp_network old_network = graph->network;
    for (size_t node = 0u; node < graph->node_count; ++node) {
        float *old_embedding = graph->nodes[node].embedding;
        float *old_importance = graph->nodes[node].embedding_importance;
        graph->nodes[node].embedding = vectors[node].embedding;
        graph->nodes[node].embedding_importance = vectors[node].importance;
        vectors[node].embedding = NULL;
        vectors[node].importance = NULL;
        free(old_embedding);
        free(old_importance);
    }

    graph->network = scratch.network;
    memset(&scratch.network, 0, sizeof(scratch.network));
    graph->neural_architecture = migration->target;

    atp_neural_migration *old_history = graph->neural_migrations;
    graph->neural_migrations = new_history;
    graph->neural_migration_count++;

    atp_network_destroy(&old_network);
    free(old_history);
    atp_free_migrated_vectors(vectors, graph->node_count);
    return ATP_OK;
}

size_t atp_graph_neural_migration_count(const atp_graph *graph) {
    return graph ? graph->neural_migration_count : 0u;
}

atp_status atp_graph_neural_migration_at(const atp_graph *graph, size_t index,
                                         atp_neural_migration *out_migration) {
    if (!graph || !out_migration) {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (index >= graph->neural_migration_count) {
        return ATP_ERR_NOT_FOUND;
    }
    *out_migration = graph->neural_migrations[index];
    return ATP_OK;
}
