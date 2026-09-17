#include "action_internal.h"

#include <string.h>

int atp_action_plan_compare(const atp_action_plan *a, const atp_action_plan *b) {
    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->step_count < b->step_count) {
        return 1;
    }
    if (a->step_count > b->step_count) {
        return -1;
    }

    for (size_t i = 0u; i < a->step_count; ++i) {
        const int token_order = strcmp(a->steps[i].token, b->steps[i].token);
        if (token_order != 0) {
            return token_order;
        }
    }

    if (a->stop_reason < b->stop_reason) {
        return -1;
    }
    if (a->stop_reason > b->stop_reason) {
        return 1;
    }
    return 0;
}