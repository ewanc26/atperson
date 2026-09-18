#ifndef ATPERSON_CORE_ACTION_INTERNAL_H
#define ATPERSON_CORE_ACTION_INTERNAL_H

/*
 * Shared internals of the action scope.
 *
 * candidates.c owns the read-only context walk and aggregation of association,
 * familiarity and support evidence into ranked atp_action_candidate values;
 * plans.c owns the bounded beam search that expands raw candidates into
 * finished atp_action_plan values; ordering.c owns the deterministic total
 * ordering over plans; guard.c owns the guarded decision layer that turns
 * raw plans into a single viable outbound decision.
 *
 * This header is scope-private and must not leak into include/atperson/.
 */

#include "../internal.h"
#include "atperson/action.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- Candidate aggregation (candidates.c) --- */

atp_status atp_graph_action_candidates(const atp_graph *graph, const char *context,
                                       atp_action_candidate *out, size_t capacity,
                                       size_t *out_count);

/* --- Plan ordering (ordering.c) --- */

int atp_action_plan_compare(const atp_action_plan *a, const atp_action_plan *b);

/* --- Bounded plan generation (plans.c) --- */

atp_action_plan_config atp_action_plan_default_config(void);

/* --- Guarded decision layer (guard.c) --- */

atp_action_guard_config atp_action_guard_default_config(void);
atp_status atp_action_decide(const atp_graph *graph, const char *context,
                             const atp_action_plan_config *plan_config,
                             const atp_action_guard_config *guard_config,
                             atp_action_decision *out_decision);

#endif