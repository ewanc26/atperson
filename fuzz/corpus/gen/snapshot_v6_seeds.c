/*
 * Snapshot v6 corpus seed generator (issue #72).
 *
 * The snapshot fuzzer drives atp_graph_load over arbitrary bytes, but its
 * committed corpus only contained v5 seeds: v6 coverage started from zero
 * on every run. This program saves real v6 snapshots (1-, 2- and 3-hidden-
 * layer topologies, with and without learned state) into the corpus so the
 * fuzzer starts from valid v6 structure and mutates within the format.
 *
 * Build against the core library, run with an output directory argument:
 *   ./snapshot-v6-seeds fuzz/corpus/snapshot
 */

#include "atperson/core.h"

#include <stdio.h>
#include <stdlib.h>

static atp_neural_architecture architecture_with(unsigned layers) {
    atp_neural_architecture architecture = {0};
    architecture.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    architecture.embedding_dim = 8u;
    architecture.input_dim = 16u;
    architecture.hidden_layer_count = layers;
    /* Widths beyond hidden_layer_count must stay zero (layout validation). */
    for (unsigned layer = 0u; layer < layers; ++layer) {
        architecture.hidden_widths[layer] = 12u - 2u * layer;
    }
    architecture.output_dim = 1u;
    return architecture;
}

static bool save_seed(const char *directory, const char *name,
                      const atp_neural_architecture *architecture, bool with_learning) {
    atp_graph_config config = atp_graph_default_config();
    atp_graph *graph = atp_graph_create_with_architecture(&config, architecture);
    if (!graph) {
        fprintf(stderr, "seed %s: create failed\n", name);
        return false;
    }

    if (with_learning) {
        /* Train a few steps so weights/importance/embeddings carry non-zero
         * learned state and the ledger records observations. */
        for (unsigned step = 0u; step < 4u; ++step) {
            atp_graph_observe_text(graph, "seed v6 corpus", "at://seed/v6");
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    const atp_status status = atp_graph_save(graph, path);
    atp_graph_destroy(graph);
    if (status != ATP_OK) {
        fprintf(stderr, "seed %s: save failed (%d)\n", name, (int)status);
        return false;
    }
    printf("wrote %s\n", path);
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <corpus-dir>\n", argv[0]);
        return EXIT_FAILURE;
    }

    bool ok = true;
    atp_neural_architecture one = architecture_with(1u);
    atp_neural_architecture two = architecture_with(2u);
    atp_neural_architecture three = architecture_with(3u);
    ok = save_seed(argv[1], "v6-one-hidden.snap", &one, false) && ok;
    ok = save_seed(argv[1], "v6-two-hidden.snap", &two, true) && ok;
    ok = save_seed(argv[1], "v6-three-hidden.snap", &three, false) && ok;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
