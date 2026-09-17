#ifndef ATPERSON_CORE_ACTION_INTERNAL_H
#define ATPERSON_CORE_ACTION_INTERNAL_H

#include "atperson/action.h"

/*
 * Action-scope private header. Owned by the src/core/action atoms; a reader
 * who holds only this header plus action.h understands the in-scope shared
 * contract. Nothing here is exported through the public include tree.
 */

/*
 * Deterministic total ordering over finished action plans. shared by the
 * bounded planner (beam pruning) and the guarded decision layer (ranked
 * viable selection) so top-N and picked-plan tie-breaking agree exactly.
 * Orders score descending, step count descending, step tokens
 * lexicographically ascending, then stop reason ascending. Pure; no
 * allocation, no mutation.
 */
int atp_action_plan_compare(const atp_action_plan *a, const atp_action_plan *b);

#endif