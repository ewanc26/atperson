/* Allocation fault-injection hooks for the atperson-core-fi build (see
 * src/core/graph/graph_internal.h). Disarmed by default; armed with a budget
 * of N successful allocations, after which the next one fails. */

#include <stdlib.h>

static long atp_fi_budget = -1; /* -1: disarmed */

void atp_fi_arm(long successes_before_failure) {
    atp_fi_budget = successes_before_failure;
}

/* Disarm; returns the unused budget (>= 0 when the armed budget was not
 * exhausted, -1 when it was never armed or the failure fired). */
long atp_fi_disarm(void) {
    const long left = atp_fi_budget;
    atp_fi_budget = -1;
    return left;
}

static int atp_fi_should_fail(void) {
    if (atp_fi_budget < 0) {
        return 0;
    }
    if (atp_fi_budget == 0) {
        atp_fi_budget = -1; /* the failure fires once, then disarms */
        return 1;
    }
    atp_fi_budget--;
    return 0;
}

void *atp_fi_malloc(size_t size) { return atp_fi_should_fail() ? NULL : malloc(size); }
void *atp_fi_calloc(size_t count, size_t size) {
    return atp_fi_should_fail() ? NULL : calloc(count, size);
}
void *atp_fi_realloc(void *pointer, size_t size) {
    return atp_fi_should_fail() ? NULL : realloc(pointer, size);
}
