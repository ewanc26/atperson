#include "publish.hpp"

#include "config.hpp"
#include "control/state.hpp"
#include "lock.hpp"
#include "outbound/action.hpp"
#include "outbound/audit.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/execute.hpp"
#include "session.hpp"
#include "writer.hpp"

#include <cstdio>
#include <ctime>
#include <memory>
#include <ostream>
#include <string>

namespace atperson {
namespace cli {
namespace {

/* The outbound lock serialises the budget read-modify-write; it is separate
 * from the daemon's long-held writer lock so publishing never blocks on, or is
 * blocked by, ingestion. */
constexpr const char *kOutboundLockName = ".outbound-lock";

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
    case OutboundExecutionOutcome::Executed:
        return OutboundAuditOutcome::Executed;
    case OutboundExecutionOutcome::DryRun:
        return OutboundAuditOutcome::DryRun;
    case OutboundExecutionOutcome::Denied:
        return OutboundAuditOutcome::Denied;
    case OutboundExecutionOutcome::Deferred:
        return OutboundAuditOutcome::Deferred;
    case OutboundExecutionOutcome::Failed:
        return OutboundAuditOutcome::Failed;
    }
    return OutboundAuditOutcome::Denied;
}

} // namespace

int run_publish(std::ostream &out, const std::filesystem::path &data_dir,
                const std::filesystem::path &policy_file, const std::filesystem::path &budget_file,
                const std::filesystem::path &control_file, const std::filesystem::path &audit_file,
                const std::filesystem::path &action_file, std::int64_t now) {
    const ControlState control = load_control_state(control_file);
    const OutboundPolicy policy = load_outbound_policy(policy_file);
    OutboundBudgetState budget = load_outbound_budget_state(budget_file);
    prune_outbound_budget_state(budget, policy, now);
    const OutboundAction action = load_outbound_action(action_file);

    const StateLock outbound_lock(data_dir, kOutboundLockName);

    /* The session is established lazily: dry runs and refusals never read
     * credentials or touch the network. */
    std::unique_ptr<WolframSession> session;
    std::unique_ptr<WolframWriter> writer;
    const auto writer_for = [&]() -> OutboundWriter & {
        if (!session) {
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                       required_env("ATPERSON_APP_PASSWORD"));
            writer = std::make_unique<WolframWriter>(*session);
        }
        return *writer;
    };

    const OutboundExecutionResult result =
        execute_outbound_action(control, policy, budget, action, writer_for, now);

    if (result.budget_recorded) {
        budget.saved_at = now;
        save_outbound_budget_state(budget, budget_file);
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
    append_outbound_audit(audit_file, entry);

    out << "outcome: " << outbound_execution_outcome_name(result.outcome) << '\n'
        << "reason: " << result.reason_code << '\n'
        << "detail: " << describe_outbound_execution(result) << '\n';
    if (result.outcome == OutboundExecutionOutcome::Executed) {
        out << "uri: " << result.written.uri << '\n' << "cid: " << result.written.cid << '\n';
    }
    out << "budget recorded: " << (result.budget_recorded ? "yes" : "no") << '\n'
        << "audit: " << audit_file.string() << '\n';

    return result.outcome == OutboundExecutionOutcome::Failed ? 1 : 0;
}

} // namespace cli
} // namespace atperson
