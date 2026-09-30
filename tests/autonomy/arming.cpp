/* Arming: the one-time setup for unattended operation, and the preflight that
 * proves it took. Offline; real policy/envelope/control files in a scratch
 * directory, injected clock. */

#include "autonomy/arming.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

constexpr std::int64_t kNow = 1789639200; /* 2026-09-17T10:00:00Z */
const char *kNowText = "2026-09-17T10:00:00Z";

struct Scratch {
    std::filesystem::path root;
    ArmPaths paths;
    explicit Scratch(const char *tag)
        : root(std::filesystem::temp_directory_path() /
               ("atperson-arming-" + std::string(tag) + "-" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        paths = {root / "policy.json", root / "control.json", root / "envelopes"};
    }
    ~Scratch() { std::filesystem::remove_all(root); }
};

PreflightEnvironment full_environment() {
    PreflightEnvironment environment;
    environment.scheduler_enabled = true;
    environment.publishing_switch = true;
    environment.has_identifier = true;
    environment.has_password = true;
    environment.has_self_did = true;
    return environment;
}

ArmRequest request_for(std::initializer_list<ArmKindSpec> kinds) {
    ArmRequest request;
    request.kinds = kinds;
    return request;
}

const PreflightCheck *find(const PreflightReport &report, const char *name) {
    for (const PreflightCheck &check : report.checks) {
        if (check.name == name) {
            return &check;
        }
    }
    return nullptr;
}

template <typename Fn> bool throws_arm_error(Fn &&fn) {
    try {
        fn();
    } catch (const ArmError &) {
        return true;
    }
    return false;
}

void test_invalid_requests_are_refused() {
    const OutboundPolicy policy = default_outbound_policy();
    const ControlState control;
    auto plan = [&](ArmRequest request) { return plan_arm(request, policy, control, kNowText); };

    assert(throws_arm_error([&] { plan(ArmRequest{}); })); /* no kinds */
    assert(throws_arm_error([&] { plan(request_for({{OutboundActionKind::Unfollow, 1, 3600}})); }));
    assert(throws_arm_error([&] { plan(request_for({{OutboundActionKind::Moderation, 1, 3600}})); }));
    assert(throws_arm_error([&] {
        plan(request_for({{OutboundActionKind::Post, 1, 3600}, {OutboundActionKind::Post, 2, 3600}}));
    }));
    assert(throws_arm_error([&] { plan(request_for({{OutboundActionKind::Post, 0, 3600}})); }));
    assert(throws_arm_error([&] { plan(request_for({{OutboundActionKind::Post, 1, 0}})); }));
    assert(throws_arm_error([&] {
        plan(request_for({{OutboundActionKind::Post, 1, kMaxOutboundWindowSeconds + 1}}));
    }));

    ArmRequest bad_id = request_for({{OutboundActionKind::Post, 1, 3600}});
    bad_id.envelope_id = "Bad_ID";
    assert(throws_arm_error([&] { plan(bad_id); }));
    ArmRequest long_id = request_for({{OutboundActionKind::Post, 1, 3600}, {OutboundActionKind::Like, 1, 3600}});
    long_id.envelope_id = std::string(60, 'a'); /* the '-actions' companion would not fit */
    assert(throws_arm_error([&] { plan(long_id); }));
    ArmRequest floors = request_for({{OutboundActionKind::Post, 1, 3600}});
    floors.min_plan_score = 1.5;
    assert(throws_arm_error([&] { plan(floors); }));
    floors.min_plan_score = 0.5;
    floors.min_support_score = -0.1;
    assert(throws_arm_error([&] { plan(floors); }));
    ArmRequest expiry = request_for({{OutboundActionKind::Post, 1, 3600}});
    expiry.expires_at = "not a time";
    assert(throws_arm_error([&] { plan(expiry); }));
}

void test_plan_is_pure_and_only_touches_requested_kinds() {
    Scratch scratch("plan");
    OutboundPolicy policy = default_outbound_policy();
    ActionBudget existing;
    existing.enabled = true;
    existing.max_in_window = 7;
    budget_for(policy, OutboundActionKind::Repost) = existing;
    const ControlState control; /* fail-closed default */

    ArmRequest request = request_for({{OutboundActionKind::Post, 3, 86400},
                                      {OutboundActionKind::Like, 20, 3600}});
    request.scope_terms = {"moon", "wolf"};
    request.min_plan_score = 0.3;
    const ArmPlan plan = plan_arm(request, policy, control, kNowText);

    assert(budget_for(plan.policy, OutboundActionKind::Post).enabled);
    assert(budget_for(plan.policy, OutboundActionKind::Post).max_in_window == 3u);
    assert(budget_for(plan.policy, OutboundActionKind::Post).min_interval_seconds == 86400 / 3 / 2);
    assert(budget_for(plan.policy, OutboundActionKind::Post).duplicate_cooldown_seconds == 86400);
    assert(budget_for(plan.policy, OutboundActionKind::Like).max_in_window == 20u);
    /* Not requested: reply stays default-off, the pre-existing repost is untouched. */
    assert(!budget_for(plan.policy, OutboundActionKind::Reply).enabled);
    assert(budget_for(plan.policy, OutboundActionKind::Repost).max_in_window == 7u);

    assert(plan.control.writes_enabled && !plan.control.dry_run && !plan.control.paused);
    assert(plan.control.approval_required); /* per-action approval stays the fallback */

    /* Scope and floors bind what it says (post), not text-less kinds (like). */
    assert(plan.envelopes.size() == 2u);
    const AuthorizationEnvelope &text_envelope = plan.envelopes[0];
    const AuthorizationEnvelope &action_envelope = plan.envelopes[1];
    assert(text_envelope.id == "autonomy" && action_envelope.id == "autonomy-actions");
    assert(text_envelope.kinds.size() == 1u && text_envelope.kinds[0].kind == OutboundActionKind::Post);
    assert(text_envelope.scope_terms.size() == 2u && text_envelope.kinds[0].min_plan_score == 0.3);
    assert(action_envelope.kinds.size() == 1u && action_envelope.kinds[0].kind == OutboundActionKind::Like);
    assert(action_envelope.scope_terms.empty() && action_envelope.kinds[0].min_plan_score == 0.0);
    assert(!text_envelope.expires_at && !action_envelope.expires_at); /* never expires by default */

    /* Purity: planning wrote nothing. */
    assert(!std::filesystem::exists(scratch.paths.policy_file));
    assert(!std::filesystem::exists(scratch.paths.control_file));
    assert(!std::filesystem::exists(scratch.paths.envelopes_dir));
    assert(!plan.changes.empty());

    /* Only text-less kinds: a single envelope under the plain id. */
    const ArmPlan likes_only = plan_arm(request_for({{OutboundActionKind::Like, 5, 3600}}), policy,
                                        control, kNowText);
    assert(likes_only.envelopes.size() == 1u && likes_only.envelopes[0].id == "autonomy");
}

void test_arm_then_preflight_is_ready_and_states_the_bounds() {
    Scratch scratch("ready");
    ArmRequest request = request_for({{OutboundActionKind::Post, 3, 86400},
                                      {OutboundActionKind::Like, 20, 3600}});
    request.scope_terms = {"moon"};
    const ArmPlan plan = plan_arm(request, default_outbound_policy(), ControlState{}, kNowText);
    apply_arm(plan, scratch.paths);

    const PreflightReport report = run_preflight(full_environment(), scratch.paths, kNow);
    assert(report.ready);
    assert(find(report, "authorization")->ok);
    assert(report.bounds.size() == 2u);
    assert(report.bounds[0].find("post: at most 3 per 1d") != std::string::npos);
    assert(report.bounds[0].find("scope: moon") != std::string::npos);
    assert(report.bounds[0].find("no expiry") != std::string::npos);
    assert(report.bounds[1].find("like: at most 20 per 1h") != std::string::npos);
    assert(report.bounds[1].find("autonomy-actions") != std::string::npos);

    /* Re-arming replaces rather than merges: drop post, keep like. */
    const ArmPlan again = plan_arm(request_for({{OutboundActionKind::Like, 5, 3600}}),
                                   load_outbound_policy(scratch.paths.policy_file),
                                   load_control_state(scratch.paths.control_file), kNowText);
    apply_arm(again, scratch.paths);
    assert(list_authorization_envelopes(scratch.paths.envelopes_dir) ==
           std::vector<std::string>{"autonomy"});
    const PreflightReport after = run_preflight(full_environment(), scratch.paths, kNow);
    /* Post is still enabled in the policy but no envelope covers it any more. */
    assert(after.ready);
    assert(find(after, "authorization")->detail.find("still needs per-action approval: post") !=
           std::string::npos);
}

void test_every_missing_link_blocks_and_is_named() {
    Scratch scratch("blockers");
    apply_arm(plan_arm(request_for({{OutboundActionKind::Post, 3, 86400}}), default_outbound_policy(),
                       ControlState{}, kNowText),
              scratch.paths);
    assert(run_preflight(full_environment(), scratch.paths, kNow).ready);

    auto blocked_by = [&](PreflightEnvironment environment, const char *check) {
        const PreflightReport report = run_preflight(environment, scratch.paths, kNow);
        return !report.ready && !find(report, check)->ok;
    };
    PreflightEnvironment environment = full_environment();
    environment.scheduler_enabled = false;
    assert(blocked_by(environment, "scheduler"));
    environment = full_environment();
    environment.publishing_switch = false;
    assert(blocked_by(environment, "publishing-switch"));
    environment = full_environment();
    environment.has_password = false;
    assert(blocked_by(environment, "credentials"));
    environment = full_environment();
    environment.has_identifier = false;
    assert(blocked_by(environment, "credentials"));

    /* The identity check is advisory: it is shown but does not block. */
    environment = full_environment();
    environment.has_self_did = false;
    const PreflightReport advisory = run_preflight(environment, scratch.paths, kNow);
    assert(advisory.ready && !find(advisory, "identity")->ok && find(advisory, "identity")->advisory);

    /* Control state: each way of being stopped is reported. */
    ControlState control = load_control_state(scratch.paths.control_file);
    for (int variant = 0; variant < 4; ++variant) {
        ControlState changed = control;
        switch (variant) {
        case 0: changed.paused = true; break;
        case 1: changed.writes_enabled = false; break;
        case 2: changed.dry_run = true; break;
        default: changed.offline_mode = true; break;
        }
        save_control_state(changed, scratch.paths.control_file);
        assert(!run_preflight(full_environment(), scratch.paths, kNow).ready);
        assert(!find(run_preflight(full_environment(), scratch.paths, kNow), "control")->ok);
    }
    save_control_state(control, scratch.paths.control_file);
    assert(run_preflight(full_environment(), scratch.paths, kNow).ready);

    /* No policy kind enabled. */
    std::ofstream(scratch.paths.policy_file, std::ios::trunc) << serialise_outbound_policy(default_outbound_policy());
    assert(!run_preflight(full_environment(), scratch.paths, kNow).ready);
    assert(!find(run_preflight(full_environment(), scratch.paths, kNow), "policy")->ok);
}

void test_preflight_reports_the_output_guard() {
    Scratch scratch("guard");
    apply_arm(plan_arm(request_for({{OutboundActionKind::Post, 3, 86400}}), default_outbound_policy(),
                       ControlState{}, kNowText),
              scratch.paths);
    scratch.paths.denylist_file = scratch.root / "denylist.txt";

    /* No terms: advisory only, still ready, and it says how to add some. */
    PreflightReport report = run_preflight(full_environment(), scratch.paths, kNow);
    assert(report.ready);
    assert(find(report, "output-guard") != nullptr && find(report, "output-guard")->advisory);
    assert(find(report, "output-guard")->detail.find("no denylist terms") != std::string::npos);

    std::ofstream(scratch.paths.denylist_file) << "alpha\nbeta\n";
    report = run_preflight(full_environment(), scratch.paths, kNow);
    assert(report.ready && find(report, "output-guard")->ok);
    assert(find(report, "output-guard")->detail.find("2 term(s)") != std::string::npos);

    /* Without a configured path the check is simply absent. */
    scratch.paths.denylist_file.clear();
    assert(find(run_preflight(full_environment(), scratch.paths, kNow), "output-guard") == nullptr);
}

void test_authorization_reflects_expiry_and_approval_mode() {
    Scratch scratch("auth");
    ArmRequest request = request_for({{OutboundActionKind::Post, 3, 86400}});
    request.expires_at = "2026-09-18T00:00:00Z"; /* the day after kNow */
    apply_arm(plan_arm(request, default_outbound_policy(), ControlState{}, kNowText), scratch.paths);

    assert(run_preflight(full_environment(), scratch.paths, kNow).ready);
    /* A day later the authorisation has lapsed and the report says why. */
    const PreflightReport lapsed = run_preflight(full_environment(), scratch.paths, kNow + 3 * 86400);
    assert(!lapsed.ready);
    assert(find(lapsed, "authorization")->detail.find("expired") != std::string::npos);

    /* With per-action approval turned off the policy limits alone apply. */
    ControlState control = load_control_state(scratch.paths.control_file);
    control.approval_required = false;
    save_control_state(control, scratch.paths.control_file);
    const PreflightReport open = run_preflight(full_environment(), scratch.paths, kNow + 3 * 86400);
    assert(open.ready);
    assert(open.bounds.front().find("per-action approval is off") != std::string::npos);
}

void test_unsatisfiable_or_broken_envelopes_never_count() {
    Scratch scratch("broken");
    apply_arm(plan_arm(request_for({{OutboundActionKind::Like, 20, 3600}}), default_outbound_policy(),
                       ControlState{}, kNowText),
              scratch.paths);
    revoke_authorization_envelope(scratch.paths.envelopes_dir, "autonomy");

    /* A hand-made envelope scoping a text-less kind can never be satisfied. */
    AuthorizationEnvelope scoped;
    scoped.id = "scoped-like";
    scoped.scope_terms = {"moon"};
    scoped.created_at = kNowText;
    EnvelopeKindRule rule;
    rule.kind = OutboundActionKind::Like;
    rule.max_in_window = 5;
    rule.window_seconds = 3600;
    scoped.kinds.push_back(rule);
    save_authorization_envelope(scoped, scratch.paths.envelopes_dir);
    PreflightReport report = run_preflight(full_environment(), scratch.paths, kNow);
    assert(!report.ready);
    assert(find(report, "authorization")->detail.find("scope") != std::string::npos);

    /* A corrupt envelope file is reported and widens nothing. */
    std::ofstream(scratch.paths.envelopes_dir / "corrupt.json") << "{ not json";
    report = run_preflight(full_environment(), scratch.paths, kNow);
    assert(!report.ready);
    assert(find(report, "authorization")->detail.find("unusable") != std::string::npos);
}

void test_disarm_stops_everything_and_is_idempotent() {
    Scratch scratch("disarm");
    apply_arm(plan_arm(request_for({{OutboundActionKind::Post, 3, 86400},
                                    {OutboundActionKind::Like, 5, 3600}}),
                       default_outbound_policy(), ControlState{}, kNowText),
              scratch.paths);
    assert(run_preflight(full_environment(), scratch.paths, kNow).ready);

    disarm(scratch.paths, "autonomy");
    assert(!load_control_state(scratch.paths.control_file).writes_enabled);
    assert(list_authorization_envelopes(scratch.paths.envelopes_dir).empty());
    assert(!run_preflight(full_environment(), scratch.paths, kNow).ready);
    /* The policy is left as configured: with writes off and no envelope nothing publishes. */
    assert(budget_for(load_outbound_policy(scratch.paths.policy_file), OutboundActionKind::Post).enabled);
    disarm(scratch.paths, "autonomy"); /* idempotent */
}

/* Applying writes the control state last: if that step fails the entity is left
 * inert, never armed without the write gate. */
void test_a_failed_apply_leaves_the_entity_inert() {
    Scratch scratch("inert");
    std::filesystem::create_directories(scratch.paths.control_file); /* a directory: the save fails */
    const ArmPlan plan = plan_arm(request_for({{OutboundActionKind::Post, 3, 86400}}),
                                  default_outbound_policy(), ControlState{}, kNowText);
    bool threw = false;
    try {
        apply_arm(plan, scratch.paths);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    assert(!run_preflight(full_environment(), scratch.paths, kNow).ready);
}

} // namespace

int main() {
    test_invalid_requests_are_refused();
    test_plan_is_pure_and_only_touches_requested_kinds();
    test_arm_then_preflight_is_ready_and_states_the_bounds();
    test_every_missing_link_blocks_and_is_named();
    test_preflight_reports_the_output_guard();
    test_authorization_reflects_expiry_and_approval_mode();
    test_unsatisfiable_or_broken_envelopes_never_count();
    test_disarm_stops_everything_and_is_idempotent();
    test_a_failed_apply_leaves_the_entity_inert();
    std::puts("arming tests passed");
    return 0;
}
