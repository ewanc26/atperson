#include "attempt.hpp"

#include "audit.hpp"
#include "budget.hpp"
#include "config.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"

#include <cstdio>
#include <ctime>

namespace atperson {
namespace {

std::string rfc3339_from_unix(std::int64_t unix_seconds) {
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

OutboundAuditOutcome audit_outcome(OutboundExecutionOutcome outcome) {
    switch (outcome) {
    case OutboundExecutionOutcome::Executed: return OutboundAuditOutcome::Executed;
    case OutboundExecutionOutcome::DryRun: return OutboundAuditOutcome::DryRun;
    case OutboundExecutionOutcome::Denied: return OutboundAuditOutcome::Denied;
    case OutboundExecutionOutcome::Deferred: return OutboundAuditOutcome::Deferred;
    case OutboundExecutionOutcome::Failed: return OutboundAuditOutcome::Failed;
    }
    return OutboundAuditOutcome::Denied;
}

JournalActionOutcome journal_action_outcome(OutboundExecutionOutcome outcome) {
    switch (outcome) {
    case OutboundExecutionOutcome::Executed: return JournalActionOutcome::Executed;
    case OutboundExecutionOutcome::DryRun: return JournalActionOutcome::DryRun;
    case OutboundExecutionOutcome::Denied: return JournalActionOutcome::Denied;
    case OutboundExecutionOutcome::Deferred: return JournalActionOutcome::Deferred;
    case OutboundExecutionOutcome::Failed: return JournalActionOutcome::Failed;
    }
    return JournalActionOutcome::Denied;
}

} // namespace

OutboundExecutionResult attempt_outbound_action(const OutboundAction &action,
                                                const OutboundAttemptPaths &paths,
                                                const OutboundWriterFactory &writer_for,
                                                std::int64_t now,
                                                const OutboundAttemptOptions &options) {
    /* Gates are reloaded from disk on every attempt: an operator pause,
     * revocation or policy edit between attempts always wins. */
    const ControlState control = load_control_state(paths.control_file);
    const OutboundPolicy policy = load_outbound_policy(paths.policy_file);
    OutboundBudgetState budget = load_outbound_budget_state(paths.budget_file);
    prune_outbound_budget_state(budget, policy, now);

    const OutboundExecutionResult result =
        execute_outbound_action(control, policy, budget, action, writer_for, now,
                                 options.attest, options.external_publishing_allowed);

    if (result.budget_recorded) {
        budget.saved_at = now;
        save_outbound_budget_state(budget, paths.budget_file);
    }

    OutboundAuditEntry entry;
    entry.at = rfc3339_from_unix(now);
    entry.kind = outbound_kind_name(action.kind);
    entry.rkey = action.rkey;
    entry.digest = action.digest;
    entry.outcome = audit_outcome(result.outcome);
    entry.reason = result.reason_code;
    entry.detail = result.detail;
    entry.uri = result.written.uri;
    entry.cid = result.written.cid;
    if (result.attestation) {
        entry.attestation_cid = result.attestation->payload_cid;
        entry.attestation_signature = result.attestation->signature_hex;
        entry.attestation_public_key = result.attestation->public_key_did;
        entry.attestation_key_id = result.attestation->key_id;
        entry.attestation_algorithm = result.attestation->algorithm;
    }
    append_outbound_audit(paths.audit_file, entry);

    /* The journal (#27) records the same attempt as durable experience
     * provenance: the rkey is the stable action id, and the executed
     * record's at-URI is what later event linkage keys on. */
    JournalAction journal_entry;
    journal_entry.id = action.rkey;
    journal_entry.kind = outbound_kind_name(action.kind);
    journal_entry.text = action.text;
    journal_entry.digest = action.digest;
    journal_entry.outcome = journal_action_outcome(result.outcome);
    journal_entry.reason = result.reason_code;
    journal_entry.uri = result.written.uri;
    journal_entry.cid = result.written.cid;
    journal_entry.at = rfc3339_from_unix(now);
    if (result.outcome == OutboundExecutionOutcome::Executed && options.mac) {
        journal_entry.mac = options.mac(action);
    }
    append_journal_action(paths.journal_file, journal_entry);

    return result;
}

} // namespace atperson
