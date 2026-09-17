#ifndef ATPERSON_AUDIT_EVIDENCE_HPP
#define ATPERSON_AUDIT_EVIDENCE_HPP

// Advisory audit: decision-trace evidence.
//
// Owns the conversion of an authoritative C23 decision trace into the
// structured JSON `state` document sent to the TypeSafe System One audit
// endpoint, plus the shared JSON ownership RAII and the decision/abstain
// reason name mappers used by the audit atoms. This is pure presentation:
// nothing here mutates learned state, planning, or network policy. The JSON
// is bounded by construction (plan tokens and steps are capped by the C core
// contract, so the serialized document cannot grow with the corpus).
//
// Failure semantics: helpers throw std::runtime_error when cJSON can no
// longer allocate; the returned cJSON ownership is exclusive (AuditJson).

#include "atperson/action.h"

#include <cJSON.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace atperson {
namespace audit {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using AuditJson = std::unique_ptr<cJSON, JsonDelete>;

/** Serialize a JSON document to an owned compact string. Returns empty when
 *  the document is empty. Throws std::runtime_error on allocation failure. */
std::string json_to_string(const AuditJson &json);

const char *stop_reason_name(atp_action_plan_stop_reason reason) noexcept;
const char *abstain_reason_name(atp_action_abstain_reason reason) noexcept;

/**
 * Build the `state` document for one guarded decision.
 *
 * Includes the guarded context, the outcome (abstain or accepted plan), the
 * stop evidence with its echoed thresholds, the accepted steps with full
 * candidate scores, and the guard/planner configuration that produced the
 * decision. Read-only evidence only; no learned state is exposed beyond the
 * bounded decision trace.
 */
AuditJson build_audit_state(const atp_action_decision_config &config,
                            const atp_action_decision &decision,
                            std::string_view context);

} // namespace audit
} // namespace atperson

#endif