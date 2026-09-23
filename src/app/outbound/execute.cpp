#include "execute.hpp"

#include "evaluate.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace atperson {
namespace {

OutboundExecutionResult denied(OutboundExecutionOutcome outcome, std::string reason_code,
                               std::string detail, const OutboundBudgetStatus &budget) {
    OutboundExecutionResult result;
    result.outcome = outcome;
    result.reason_code = std::move(reason_code);
    result.detail = std::move(detail);
    result.budget = budget;
    return result;
}

const char *control_denial_code(const ControlState &control, const OutboundAction &action) {
    if (!control.writes_enabled) {
        return "writes_disabled";
    }
    if (control.approval_required && !is_digest_approved(control, action.digest)) {
        return "approval_required";
    }
    return "control_refused";
}

} // namespace

const char *outbound_execution_outcome_name(OutboundExecutionOutcome outcome) noexcept {
    switch (outcome) {
    case OutboundExecutionOutcome::Executed:
        return "executed";
    case OutboundExecutionOutcome::DryRun:
        return "dry_run";
    case OutboundExecutionOutcome::Denied:
        return "denied";
    case OutboundExecutionOutcome::Deferred:
        return "deferred";
    case OutboundExecutionOutcome::Failed:
        return "failed";
    }
    return "denied";
}

OutboundExecutionResult
execute_outbound_action(const ControlState &control, const OutboundPolicy &policy,
                        OutboundBudgetState &budget, const OutboundAction &action,
                        const OutboundWriterFactory &writer_for, std::int64_t now,
                        const OutboundAttestationFactory &attest,
                        bool external_publishing_allowed) {
    const auto proposal = outbound_action_proposal(action);
    const OutboundBudgetStatus budget_status =
        outbound_budget_status(policy, budget, action.kind, now);

    /* 1. Operator pause: refuse before any evaluation. Learned state is
     * untouched and the action text is never touched. */
    if (control.paused) {
        return denied(OutboundExecutionOutcome::Denied, "paused",
                      "runtime is paused; no outbound action may be attempted", budget_status);
    }

    /* 2. #23 outbound policy: rate limits, duplicates and default-deny. */
    const OutboundPolicyDecision decision = evaluate_outbound_policy(policy, budget, proposal, now);
    if (decision.outcome == OutboundOutcome::Deny) {
        return denied(OutboundExecutionOutcome::Denied, outbound_reason_code(decision.reason),
                      describe_outbound_decision(decision), decision.budget);
    }
    if (decision.outcome == OutboundOutcome::Defer) {
        return denied(OutboundExecutionOutcome::Deferred, outbound_reason_code(decision.reason),
                      describe_outbound_decision(decision), decision.budget);
    }

    /* 3. Dry-run: the policy says yes, but this run must not write. */
    if (control.dry_run) {
        return denied(OutboundExecutionOutcome::DryRun, "dry_run",
                      "policy allows this action, but dry-run mode is on", decision.budget);
    }

    /* 4. #22 control gate: the documented choke point for every write. */
    try {
        ensure_outbound_allowed(control, action.digest);
    } catch (const std::exception &error) {
        return denied(OutboundExecutionOutcome::Denied, control_denial_code(control, action),
                      error.what(), decision.budget);
    }

    /* Environment permission is an independent master switch. It cannot
     * bypass policy, dry-run, pause, or digest approval; it only prevents a
     * process from reaching the network when publishing was not explicitly
     * enabled by the operator. */
    if (!external_publishing_allowed) {
        return denied(OutboundExecutionOutcome::Denied, "external_publishing_disabled",
                      "external publishing is disabled; set ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true",
                      decision.budget);
    }

    /* 5. Network write, only now. Resolve reply parent/root CIDs from the
     * actual records, then put the frozen body under the frozen rkey. */
    try {
        OutboundWriter &writer = writer_for();
        std::string root_cid;
        std::string parent_cid;
        if (outbound_action_is_reply(action)) {
            root_cid = writer.resolve_record_cid(action.reply_root);
            parent_cid = writer.resolve_record_cid(action.reply_parent);
        }
        const std::string record_json = build_outbound_record_json(action, root_cid, parent_cid);
        std::optional<OutboundAttestation> attestation;
        if (attest) {
            try {
                attestation = attest(record_json, action);
            } catch (const std::exception &error) {
                return denied(OutboundExecutionOutcome::Failed, "attestation_failed", error.what(),
                              decision.budget);
            }
        }
        OutboundWriteResult written =
            writer.put_record(kOutboundPostCollection, action.rkey, record_json);
        record_outbound_action(budget, proposal, budget_for(policy, action.kind), now);

        OutboundExecutionResult result;
        result.outcome = OutboundExecutionOutcome::Executed;
        result.reason_code = "allow";
        result.detail = "action written";
        result.written = std::move(written);
        result.attestation = std::move(attestation);
        result.budget_recorded = true;
        result.budget = decision.budget;
        return result;
    } catch (const std::exception &error) {
        return denied(OutboundExecutionOutcome::Failed, "write_failed", error.what(),
                      decision.budget);
    }
}

std::string describe_outbound_execution(const OutboundExecutionResult &result) {
    switch (result.outcome) {
    case OutboundExecutionOutcome::Executed:
        return "action executed";
    case OutboundExecutionOutcome::DryRun:
        return "dry run: " + result.detail;
    case OutboundExecutionOutcome::Denied:
        return "denied: " + result.detail;
    case OutboundExecutionOutcome::Deferred:
        return "deferred: " + result.detail;
    case OutboundExecutionOutcome::Failed:
        return result.reason_code == "attestation_failed" ? "attestation failed: " + result.detail
                                                            : "write failed: " + result.detail;
    }
    return result.detail;
}

} // namespace atperson
