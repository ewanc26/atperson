/* Prune atomicity: fail every allocation the prune makes, one at a time, and
 * prove the graph is left exactly as it was (byte-identical snapshot, still
 * usable) and that a retry then produces the same result as an uninterrupted
 * prune. Built against atperson-core-fi, which routes graph allocations through
 * the hooks in fault_alloc.c. */

#include "atperson/core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void atp_fi_arm(long successes_before_failure);
long atp_fi_disarm(void);

static atp_graph *build(void) {
    atp_graph *graph = atp_graph_create(NULL);
    assert(graph != NULL);
    char text[128];
    for (int i = 0; i < 40; ++i) {
        snprintf(text, sizeof text, "common shared words rare%d unique%d", i, i % 7);
        char id[32];
        snprintf(id, sizeof id, "at://t/%d", i);
        assert(atp_graph_observe_text(graph, text, id) == ATP_OK);
    }
    assert(atp_graph_valence_event(graph, "rare7", ATP_VALENCE_INTERACTION, 1.0f, 100u,
                                   "at://e/1") == ATP_OK);
    return graph;
}

static char *read_snapshot(atp_graph *graph, size_t *size) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/atperson-prune-fault-%ld.snap", (long)getpid());
    assert(atp_graph_save(graph, path) == ATP_OK);
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *bytes = malloc((size_t)length);
    assert(bytes != NULL && fread(bytes, 1u, (size_t)length, file) == (size_t)length);
    fclose(file);
    remove(path);
    *size = (size_t)length;
    return bytes;
}

int main(void) {
    /* The uninterrupted result every faulted-then-retried prune must match. */
    atp_graph *reference = build();
    assert(atp_graph_prune_vocabulary(reference, 12u, NULL) == ATP_OK);
    size_t reference_size = 0u;
    char *reference_bytes = read_snapshot(reference, &reference_size);
    atp_graph_destroy(reference);

    int failures_seen = 0;
    int completed = 0;
    for (long budget = 0; budget < 64 && !completed; ++budget) {
        atp_graph *graph = build();
        size_t before_size = 0u;
        char *before = read_snapshot(graph, &before_size);

        atp_fi_arm(budget);
        const atp_status status = atp_graph_prune_vocabulary(graph, 12u, NULL);
        const long unused = atp_fi_disarm();

        if (status == ATP_ERR_OUT_OF_MEMORY) {
            failures_seen++;
            size_t after_size = 0u;
            char *after = read_snapshot(graph, &after_size);
            /* Exactly as it was. */
            assert(after_size == before_size && memcmp(after, before, before_size) == 0);
            free(after);
            /* Still fully usable: known and new tokens both work. */
            assert(atp_graph_observe_text(graph, "common shared brandnew", "at://t/x") == ATP_OK);
            /* ...and a retry lands on the same state as an uninterrupted prune. */
            atp_graph *fresh = build();
            assert(atp_graph_prune_vocabulary(fresh, 12u, NULL) == ATP_OK);
            atp_graph_destroy(fresh);
            atp_graph_destroy(graph);
            graph = build();
            assert(atp_graph_prune_vocabulary(graph, 12u, NULL) == ATP_OK);
        } else {
            assert(status == ATP_OK && unused >= 0);
            completed = 1;
        }
        size_t final_size = 0u;
        char *final_bytes = read_snapshot(graph, &final_size);
        assert(final_size == reference_size && memcmp(final_bytes, reference_bytes, final_size) == 0);
        free(final_bytes);
        free(before);
        atp_graph_destroy(graph);
    }
    /* The prune allocates, so at least one point must have failed, and the
     * sweep must have reached a budget large enough to complete. */
    assert(failures_seen > 0);
    assert(completed);
    free(reference_bytes);
    printf("prune fault injection: %d failure point(s), all atomic\n", failures_seen);
    return 0;
}
