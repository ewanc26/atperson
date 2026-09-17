#ifndef ATPERSON_AUDIT_VERDICT_HPP
#define ATPERSON_AUDIT_VERDICT_HPP

// Advisory audit: response parsing and report rendering.
//
// Owns the typed view of a TypeSafe System One response (`AuditVerdict`) and
// the deterministic report renderer. Parsing is strict: unexpected or missing
// fields throw so a changed remote contract cannot silently weaken the audit.
// Rendering prints the authoritative C23 decision trace first, then the
// advisory typeSafe block with a derived, purely descriptive summary line.
// Nothing here mutates learned state, planning, or network policy.
//
// Score probabilities are reordered by ascending level number so rendering is
// stable regardless of the remote response's key order.

#include "audit/evidence.hpp"

#include "atperson/action.h"

#include <cJSON.h>

#include <cstdint>
#include <ostream>
#include <optional>
#include <string>
#include <vector>

namespace atperson {
namespace audit {

struct ScoreLevel {
    std::string key;
    std::string label;
    double probability;
};

struct ScoreAnswer {
    double score;
    double confidence;
    std::vector<ScoreLevel> levels;
};

/** One parsed audit response. Absent answers keep their optional empty. */
struct AuditVerdict {
    std::string model;
    bool plan_outcome;
    std::optional<double> first_step_credible;
    std::optional<double> plan_coherent;
    std::optional<double> abstain_expected;
    std::optional<ScoreAnswer> evidence_quality;
    std::int64_t input_tokens{};
    std::int64_t output_tokens{};
};

/** Parse a TypeSafe System One response document. Throws on malformed JSON. */
AuditVerdict parse_audit_response(cJSON *response);

/** Render the compact authoritative decision trace (learned-core evidence). */
void render_decision_trace(std::ostream &out, const atp_action_decision &decision);

/** Render the advisory TypeSafe verdict block and its derived summary. */
void render_audit_verdict(std::ostream &out, const AuditVerdict &verdict);

/** Render the full audit report: authoritative trace then advisory block. */
void render_audit_report(std::ostream &out, const atp_action_decision &decision,
                         const AuditVerdict &verdict);

} // namespace audit
} // namespace atperson

#endif