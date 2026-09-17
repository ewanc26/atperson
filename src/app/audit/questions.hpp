#ifndef ATPERSON_AUDIT_QUESTIONS_HPP
#define ATPERSON_AUDIT_QUESTIONS_HPP

// Advisory audit: question schema.
//
// Owns the TypeSafe `questions` map used by the audit command. The rubric is
// fixed and deterministic: the question set depends only on the decision
// outcome (accepted plan vs abstention), never on learned state. Noul
// questions probe one narrow claim; the Score rates evidence solidity on
// ordered levels. Question ids are code-only and are not sent to the model;
// the instructions/criteria carry full meaning. Pure presentation, read-only.
//
// Failure semantics: throws std::runtime_error when cJSON cannot allocate.

#include "audit/evidence.hpp"

namespace atperson {
namespace audit {

/** Build the `questions` map for a decision with the given outcome. */
AuditJson build_audit_questions(bool plan_outcome);

} // namespace audit
} // namespace atperson

#endif