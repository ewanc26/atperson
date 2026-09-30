#ifndef ATPERSON_AUTONOMY_ARMING_HPP
#define ATPERSON_AUTONOMY_ARMING_HPP

// Arming: the one-time setup that lets the entity act on its own afterwards.
//
// Autonomous operation already exists as separate primitives: the scheduler
// (decides and freezes proposals), the outbound policy (per-kind default-off
// budgets), the control state (pause / writes / dry-run) and standing
// authorization envelopes (bounded pre-approval). Enabling it used to mean
// editing all of them by hand and hoping none was forgotten, and a missing
// piece left the entity silently inert. This module is the coherent setup step
// and the check that proves it took:
//
//   plan_arm / apply_arm   compute, then durably write, the policy, envelope and
//                          control changes that authorise a stated set of
//                          action kinds within stated ceilings. Planning is
//                          pure; applying writes the envelope and policy first
//                          and the control state (writes on) LAST, so a failure
//                          part-way leaves the entity inert, never half-armed.
//   disarm                 revokes the envelope and turns writes off.
//   run_preflight          read-only end-to-end readiness report: every link
//                          in the unattended chain, what is blocking, and the
//                          exact bounds it will act within.
//
// Nothing here widens any gate. The envelope is validated against the policy by
// the real envelope parser, so it can only narrow it; pause, dry-run, the
// publishing master switch and the policy's own rate limits still apply, and the
// operator's kill switches (`control pause`, remote pause) keep working. "No
// human in the loop" means no per-action approval after setup, not no bounds:
// the ceilings chosen at setup are enforced on every action forever.
//
// Pure apart from the explicit load/save calls; `now` is always injected.
//
// Failure modes: ArmError for a request that is not a valid arming (no kinds,
// non-executable kind, impossible ceiling, bad envelope id, an envelope the
// parser would reject); std::runtime_error for I/O failure.

#include "../control/envelope.hpp"
#include "../control/state.hpp"
#include "../outbound/actions.hpp"
#include "../outbound/config.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

class ArmError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* One action kind the entity may perform on its own, and its ceiling. */
struct ArmKindSpec {
    OutboundActionKind kind{OutboundActionKind::Post};
    std::uint32_t max_in_window{1u};
    std::int64_t window_seconds{24ll * 60ll * 60ll};
};

struct ArmRequest {
    std::vector<ArmKindSpec> kinds;
    /* When non-empty the action text must contain one of these terms. Scope and
     * the score floors constrain what the entity *says*, so they apply to
     * post and reply only: a like, repost or follow carries no text and no
     * decision evidence, so a scope or floor could never be satisfied and would
     * silently leave it waiting for approval. Those kinds get their own
     * envelope bounded by their ceilings (and expiry) alone. */
    std::vector<std::string> scope_terms;
    /* Decision-evidence floors in [0, 1]; 0 means none. */
    double min_plan_score{0.0};
    double min_support_score{0.0};
    /* RFC 3339 UTC. nullopt = the authorisation never expires, which is what
     * "acts on its own after setup" means; revoke it with `disarm`. */
    std::optional<std::string> expires_at;
    /* Id of the envelope for post/reply; kinds without text go in
     * "<id>-actions" (or in "<id>" itself when no text kind is requested). */
    std::string envelope_id{"autonomy"};
};

struct ArmPaths {
    std::filesystem::path policy_file;
    std::filesystem::path control_file;
    std::filesystem::path envelopes_dir;
};

/* The changes arming would make, computed without touching disk. */
struct ArmPlan {
    OutboundPolicy policy;
    ControlState control;
    /* The id this arming was requested under; applying replaces any envelope
     * from an earlier arm under it (and its "-actions" companion). */
    std::string base_id;
    /* One or two envelopes: text kinds (scope/floors) and textless kinds. */
    std::vector<AuthorizationEnvelope> envelopes;
    /* Human-readable list of what changes, for the dry-run preview. */
    std::vector<std::string> changes;
};

/* Only kinds the executor can perform are allowed: post, reply, like, repost,
 * follow. Throws ArmError for anything invalid. The returned envelope has been
 * round-tripped through the real parser against the resulting policy. */
[[nodiscard]] ArmPlan plan_arm(const ArmRequest &request, const OutboundPolicy &current_policy,
                               const ControlState &current_control, std::string_view now_rfc3339);

/* Write the plan: envelope, then policy, then control (writes enabled last). */
void apply_arm(const ArmPlan &plan, const ArmPaths &paths);

/* Ids of the envelopes arming with `envelope_id` may have written. */
[[nodiscard]] std::vector<std::string> arm_envelope_ids(std::string_view envelope_id);

/* Revoke the envelope(s) and disable writes. Idempotent. The policy is left as it
 * is: with writes off and no envelope nothing can be published. */
void disarm(const ArmPaths &paths, std::string_view envelope_id);

/* What the process environment contributes. Values are never carried, only
 * whether they are set. */
struct PreflightEnvironment {
    bool scheduler_enabled{false};   /* ATPERSON_SCHEDULER=1 */
    bool publishing_switch{false};   /* ATPERSON_ALLOW_EXTERNAL_PUBLISHING true */
    bool has_identifier{false};      /* ATPERSON_IDENTIFIER */
    bool has_password{false};        /* ATPERSON_APP_PASSWORD */
    bool has_self_did{false};        /* ATPERSON_SELF_DID (self-post exclusion) */
};

struct PreflightCheck {
    std::string name;
    bool ok{false};
    /* A non-blocking observation (shown, but does not stop READY). */
    bool advisory{false};
    std::string detail;
};

struct PreflightReport {
    std::vector<PreflightCheck> checks;
    /* One line per kind the entity may perform unattended, with the bounds it
     * acts within. Empty when nothing is authorised. */
    std::vector<std::string> bounds;
    /* True only when every non-advisory check passed. */
    bool ready{false};
};

[[nodiscard]] PreflightReport run_preflight(const PreflightEnvironment &environment,
                                            const ArmPaths &paths, std::int64_t now_unix);

} // namespace atperson

#endif
