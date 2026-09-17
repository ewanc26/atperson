/* Fuzz target: snapshot loading (issue #11).
 *
 * The snapshot decoder processes untrusted bytes from disk. This target
 * writes the fuzzer input to a temp file and drives atp_graph_load over
 * it: malformed lengths, truncated records, and hostile field values must
 * be rejected with a status, never crash, leak, or read out of bounds.
 *
 * The graph, when one loads, is exercised: associations and action
 * candidates run against the loaded state so decoder-trusted values
 * (token bytes, counts, scores) are consumed downstream too. */

#include "atperson/action.h"
#include "atperson/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    char path[] = "/tmp/atperson-fuzz-snap-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return 0;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        remove(path);
        return 0;
    }
    fwrite(data, 1u, size, file);
    fclose(file);

    atp_status status = ATP_OK;
    atp_graph *graph = atp_graph_load(path, &status);
    remove(path);
    if (!graph) {
        /* Rejection is the expected outcome for malformed input. */
        return 0;
    }

    /* A loaded graph must be safe to query. Queries use fixed literals so
     * the target exercises the lookup paths over decoder-trusted values
     * (token bytes, counts, scores), not the query parser itself. */
    atp_association associations[8u];
    size_t count = 0u;
    (void)atp_graph_associations(graph, "a", associations, 8u, &count);

    atp_action_candidate candidates[8u];
    (void)atp_graph_action_candidates(graph, "a", candidates, 8u, &count);

    const atp_graph_stats stats = atp_graph_get_stats(graph);
    (void)stats;

    atp_graph_destroy(graph);
    return 0;
}
