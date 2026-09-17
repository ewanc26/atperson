/* Outbound action policy and rate budgets (#23): default-deny, per-window
 * limits, cooldowns, duplicate suppression, conservative restart, and
 * boundary default-deny for unknown kinds. All offline; `now` is injected. */
#include "cli/outbound.hpp"
#include "control/state.hpp"
#include "outbound/actions.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using atperson::ActionBudget;
using atperson::OutboundActionKind;
using atperson::OutboundActionProposal;
using atperson::OutboundBudgetState;
using atperson::OutboundOutcome;
using atperson::OutboundPolicy;
using atperson::OutboundPolicyDecision;
using atperson::OutboundReason;

constexpr std::int64_t HOUR = 3600;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-outbound-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void write_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

OutboundActionProposal proposal(OutboundActionKind kind, const char *target, const char *digest) {
    OutboundActionProposal value;
    value.kind = kind;
    value.target = target;
    value.action_digest = digest;
    return value;
}

/* A policy with exactly one kind enabled; every other kind stays default-deny. */
OutboundPolicy policy_with(OutboundActionKind kind, const ActionBudget &budget) {
    OutboundPolicy policy;
    atperson::budget_for(policy, kind) = budget;
    return policy;
}

void test_kind_names_round_trip() {
    for (std::size_t index = 0; index < atperson::kOutboundActionKindCount; ++index) {
        const auto kind = static_cast<OutboundActionKind>(index);
        const auto parsed = atperson::parse_outbound_kind(atperson::outbound_kind_name(kind));
        assert(parsed.has_value());
        assert(*parsed == kind);
    }
    assert(!atperson::parse_outbound_kind("").has_value());
    assert(!atperson::parse_outbound_kind("Post").has_value());
    assert(!atperson::parse_outbound_kind("nsfw").has_value());
    std::printf("ok kind names round-trip\n");
}

void test_default_policy_denies_every_kind() {
    const OutboundPolicy policy = atperson::default_outbound_policy();
    const OutboundBudgetState state;
    for (std::size_t index = 0; index < atperson::kOutboundActionKindCount; ++index) {
        const auto kind = static_cast<OutboundActionKind>(index);
        const auto decision = atperson::evaluate_outbound_policy(
            policy, state, proposal(kind, "at://did:plc:other/app.bsky.feed.post/1", "d"), 1000);
        assert(decision.outcome == OutboundOutcome::Deny);
        assert(decision.reason == OutboundReason::KindDisabled);
        assert(!decision.budget.enabled);
    }
    std::printf("ok default policy denies every kind\n");
}

void test_policy_round_trip() {
    OutboundPolicy policy;
    ActionBudget post;
    post.enabled = true;
    post.max_in_window = 5u;
    post.window_seconds = 3600;
    post.min_interval_seconds = 120;
    post.duplicate_cooldown_seconds = 86400;
    atperson::budget_for(policy, OutboundActionKind::Post) = post;

    const std::string json = atperson::serialise_outbound_policy(policy);
    const OutboundPolicy parsed = atperson::parse_outbound_policy(json, "test");
    for (std::size_t index = 0; index < atperson::kOutboundActionKindCount; ++index) {
        const auto kind = static_cast<OutboundActionKind>(index);
        const ActionBudget &a = atperson::budget_for(policy, kind);
        const ActionBudget &b = atperson::budget_for(parsed, kind);
        assert(a.enabled == b.enabled);
        assert(a.max_in_window == b.max_in_window);
        assert(a.window_seconds == b.window_seconds);
        assert(a.min_interval_seconds == b.min_interval_seconds);
        assert(a.duplicate_cooldown_seconds == b.duplicate_cooldown_seconds);
    }
    std::printf("ok policy round-trips\n");
}

void test_policy_rejects_bad_documents() {
    const std::vector<std::string> bad = {
        R"({"format":"wrong","version":1,"kinds":{}})",
        R"({"format":"atperson-outbound-policy","version":2,"kinds":{}})",
        R"({"format":"atperson-outbound-policy","version":1})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"nsfw":{"enabled":true}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{"enabled":true,"window_seconds":0}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{"enabled":true,"max_in_window":0}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{"enabled":true,"min_interval_seconds":-1}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{"enabled":true,"window_seconds":1.5}}})",
        R"({"format":"atperson-outbound-policy","version":1,"kinds":{"post":{"enabled":true},"post":{"enabled":false}}})",
    };
    for (const std::string &document : bad) {
        bool threw = false;
        try {
            (void)atperson::parse_outbound_policy(document, "test");
        } catch (const atperson::OutboundPolicyError &) {
            threw = true;
        }
        assert(threw);
    }
    std::printf("ok policy rejects bad documents\n");
}

void test_missing_policy_file_is_default_deny() {
    const auto dir = scratch_dir("missing-policy");
    const OutboundPolicy policy = atperson::load_outbound_policy(dir / "absent.json");
    assert(atperson::budget_for(policy, OutboundActionKind::Post).enabled == false);
    std::printf("ok missing policy file is default-deny\n");
}

void test_allow_then_window_exhaustion() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 1u;
    budget.window_seconds = 100;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Post, budget);
    OutboundBudgetState state;
    const auto action = proposal(OutboundActionKind::Post, "", "d1");

    const auto first = atperson::admit_outbound_action(policy, state, action, 1000);
    assert(first.outcome == OutboundOutcome::Allow);
    assert(first.budget.used_in_window == 0u);

    const auto second = atperson::admit_outbound_action(policy, state, action, 1000);
    assert(second.outcome == OutboundOutcome::Defer);
    assert(second.reason == OutboundReason::WindowExhausted);
    assert(second.retry_after_seconds > 0);
    assert(second.budget.used_in_window == 1u);

    /* After the first action ages out of the trailing window it may run again. */
    const auto third = atperson::admit_outbound_action(policy, state, action, 1000 + 101);
    assert(third.outcome == OutboundOutcome::Allow);
    std::printf("ok window exhaustion and recovery\n");
}

void test_min_interval_cooldown() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 5u;
    budget.window_seconds = 1000;
    budget.min_interval_seconds = 60;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Reply, budget);
    OutboundBudgetState state;
    const auto action = proposal(OutboundActionKind::Reply, "at://x", "d1");

    assert(atperson::admit_outbound_action(policy, state, action, 0).outcome ==
           OutboundOutcome::Allow);

    const auto too_soon = atperson::admit_outbound_action(policy, state, action, 30);
    assert(too_soon.outcome == OutboundOutcome::Defer);
    assert(too_soon.reason == OutboundReason::CooldownActive);
    assert(too_soon.retry_after_seconds == 30);

    assert(atperson::admit_outbound_action(policy, state, action, 60).outcome ==
           OutboundOutcome::Allow);
    std::printf("ok minimum interval cooldown\n");
}

void test_duplicate_suppression() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 10u;
    budget.window_seconds = 1000;
    budget.duplicate_cooldown_seconds = 120;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Like, budget);
    OutboundBudgetState state;

    const auto like_a = proposal(OutboundActionKind::Like, "at://a", "d1");
    const auto like_b = proposal(OutboundActionKind::Like, "at://b", "d1");

    assert(atperson::admit_outbound_action(policy, state, like_a, 0).outcome ==
           OutboundOutcome::Allow);

    const auto repeat = atperson::admit_outbound_action(policy, state, like_a, 10);
    assert(repeat.outcome == OutboundOutcome::Defer);
    assert(repeat.reason == OutboundReason::DuplicateSuppressed);
    assert(repeat.retry_after_seconds == 110);

    /* A different target is a different identity and is not suppressed. */
    assert(atperson::admit_outbound_action(policy, state, like_b, 10).outcome ==
           OutboundOutcome::Allow);

    /* The original may run again after its duplicate cooldown. */
    assert(atperson::admit_outbound_action(policy, state, like_a, 120).outcome ==
           OutboundOutcome::Allow);
    std::printf("ok duplicate suppression\n");
}

void test_backwards_clock_is_conservative() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 10u;
    budget.window_seconds = 1000;
    budget.min_interval_seconds = 60;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Follow, budget);
    OutboundBudgetState state;
    const auto action = proposal(OutboundActionKind::Follow, "did:plc:other", "d1");

    assert(atperson::admit_outbound_action(policy, state, action, 1000).outcome ==
           OutboundOutcome::Allow);

    /* A clock that jumps backwards must not reset the cooldown. */
    const auto jumped = atperson::evaluate_outbound_policy(policy, state, action, 500);
    assert(jumped.outcome == OutboundOutcome::Defer);
    assert(jumped.reason == OutboundReason::CooldownActive);

    const auto recorded = atperson::admit_outbound_action(policy, state, action, 500);
    assert(recorded.reason == OutboundReason::CooldownActive);
    const auto last = atperson::last_outbound_action_at(state, OutboundActionKind::Follow);
    assert(last.has_value() && *last == 1000);
    std::printf("ok backwards clock is conservative\n");
}

void test_restart_preserves_budgets() {
    const auto dir = scratch_dir("restart");
    const auto path = dir / "outbound-budget.json";

    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 1u;
    budget.window_seconds = HOUR;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Post, budget);

    OutboundBudgetState state;
    const auto action = proposal(OutboundActionKind::Post, "", "d1");
    assert(atperson::admit_outbound_action(policy, state, action, 0).outcome ==
           OutboundOutcome::Allow);
    state.saved_at = 0;
    atperson::save_outbound_budget_state(state, path);

    /* A fresh process reloads the window and cannot burst. */
    OutboundBudgetState reloaded = atperson::load_outbound_budget_state(path);
    const auto after_restart = atperson::admit_outbound_action(policy, reloaded, action, 10);
    assert(after_restart.outcome == OutboundOutcome::Defer);
    assert(after_restart.reason == OutboundReason::WindowExhausted);
    const auto last = atperson::last_outbound_action_at(reloaded, OutboundActionKind::Post);
    assert(last.has_value() && *last == 0);
    std::printf("ok restart preserves budgets\n");
}

void test_missing_and_corrupt_budget_is_fail_closed() {
    const auto dir = scratch_dir("corrupt");
    const OutboundBudgetState missing = atperson::load_outbound_budget_state(dir / "absent.json");
    assert(missing.usage[0].recent_actions.empty());

    const auto path = dir / "outbound-budget.json";
    write_file(path, "{\"format\":\"atperson-outbound-budget\",\"version\":1,\"kinds\":");
    bool threw = false;
    try {
        (void)atperson::load_outbound_budget_state(path);
    } catch (const atperson::OutboundBudgetError &) {
        threw = true;
    }
    assert(threw);

    write_file(path, "{\"format\":\"atperson-outbound-budget\",\"version\":1,\"kinds\":{"
                     "\"post\":{\"recent_actions\":[5,4]}}}");
    threw = false;
    try {
        (void)atperson::load_outbound_budget_state(path);
    } catch (const atperson::OutboundBudgetError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok missing and corrupt budget state\n");
}

void test_concurrent_proposals_serialise_at_admission() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 1u;
    budget.window_seconds = HOUR;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Repost, budget);
    OutboundBudgetState state;
    const auto action = proposal(OutboundActionKind::Repost, "at://x", "d1");

    /* Two callers evaluate the same state before either records: both see an
     * available budget. Admission is the serialisation point: the first wins,
     * the second is deferred by the limit. */
    assert(atperson::evaluate_outbound_policy(policy, state, action, 5).outcome ==
           OutboundOutcome::Allow);
    assert(atperson::evaluate_outbound_policy(policy, state, action, 5).outcome ==
           OutboundOutcome::Allow);

    assert(atperson::admit_outbound_action(policy, state, action, 5).outcome ==
           OutboundOutcome::Allow);
    const auto second = atperson::admit_outbound_action(policy, state, action, 5);
    assert(second.outcome == OutboundOutcome::Defer);
    assert(atperson::count_outbound_actions_in_window(state, OutboundActionKind::Repost, HOUR, 5) ==
           1u);
    std::printf("ok concurrent proposals serialise at admission\n");
}

void test_records_are_bounded_and_pruned() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 100000u;
    budget.window_seconds = HOUR;
    budget.duplicate_cooldown_seconds = 10;
    const OutboundPolicy policy = policy_with(OutboundActionKind::Like, budget);
    OutboundBudgetState state;

    for (int i = 0; i < 300; ++i) {
        const auto action = proposal(OutboundActionKind::Like, "at://x", std::to_string(i).c_str());
        assert(atperson::admit_outbound_action(policy, state, action, i).outcome ==
               OutboundOutcome::Allow);
    }
    assert(state.usage[static_cast<std::size_t>(OutboundActionKind::Like)].recent_actions.size() <=
           atperson::kMaxOutboundRecordsPerKind);
    assert(
        state.usage[static_cast<std::size_t>(OutboundActionKind::Like)].recent_duplicates.size() <=
        atperson::kMaxOutboundRecordsPerKind);

    atperson::prune_outbound_budget_state(state, policy, 300 + HOUR + 11);
    assert(state.usage[static_cast<std::size_t>(OutboundActionKind::Like)].recent_actions.empty());
    assert(
        state.usage[static_cast<std::size_t>(OutboundActionKind::Like)].recent_duplicates.empty());
    std::printf("ok budget records are bounded and pruned\n");
}

void test_unsupported_kind_is_denied() {
    const OutboundPolicyDecision decision = atperson::unsupported_outbound_decision();
    assert(decision.outcome == OutboundOutcome::Deny);
    assert(decision.reason == OutboundReason::UnsupportedKind);
    assert(std::string(atperson::outbound_reason_code(decision.reason)) == "unsupported_kind");
    std::printf("ok unsupported kind is denied\n");
}

void test_cli_surface() {
    const auto dir = scratch_dir("cli");
    const auto policy_file = dir / "outbound-policy.json";
    const auto budget_file = dir / "outbound-budget.json";
    const auto control_file = dir / "control-state.json";

    /* No policy: default-deny with an inspectable reason code. */
    {
        std::ostringstream out;
        const std::vector<std::string_view> args{"post"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "evaluate",
                                            args, 1000);
        const std::string text = out.str();
        assert(text.find("outcome: deny") != std::string::npos);
        assert(text.find("reason: kind_disabled") != std::string::npos);
        assert(text.find("control gate: blocked") != std::string::npos);
        assert(!std::filesystem::exists(budget_file));
    }

    /* Unknown kind is denied at the boundary, still with no budget write. */
    {
        std::ostringstream out;
        const std::vector<std::string_view> args{"nsfw"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "evaluate",
                                            args, 1000);
        const std::string text = out.str();
        assert(text.find("outcome: deny") != std::string::npos);
        assert(text.find("reason: unsupported_kind") != std::string::npos);
    }

    /* Enable one kind; evaluate allows, admit consumes budget once. */
    OutboundPolicy policy;
    ActionBudget post;
    post.enabled = true;
    post.max_in_window = 1u;
    post.window_seconds = HOUR;
    atperson::budget_for(policy, OutboundActionKind::Post) = post;
    write_file(policy_file, atperson::serialise_outbound_policy(policy));

    {
        std::ostringstream out;
        const std::vector<std::string_view> args{"post"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "evaluate",
                                            args, 1000);
        assert(out.str().find("outcome: allow") != std::string::npos);
        assert(!std::filesystem::exists(budget_file));
    }
    {
        std::ostringstream out;
        const std::vector<std::string_view> args{"post"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "admit",
                                            args, 1000);
        const std::string text = out.str();
        assert(text.find("outcome: allow") != std::string::npos);
        assert(text.find("recorded: yes") != std::string::npos);
        assert(std::filesystem::exists(budget_file));
    }
    {
        std::ostringstream out;
        const std::vector<std::string_view> args{"post"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "admit",
                                            args, 1010);
        const std::string text = out.str();
        assert(text.find("outcome: defer") != std::string::npos);
        assert(text.find("reason: window_exhausted") != std::string::npos);
        assert(text.find("recorded: no") != std::string::npos);
    }

    /* An armed control state is reported as a passing gate, without enabling
     * any network behaviour here. */
    {
        atperson::ControlState control;
        control.writes_enabled = true;
        control.dry_run = false;
        control.approval_required = false;
        atperson::save_control_state(control, control_file);
        std::ostringstream out;
        const std::vector<std::string_view> args{"post"};
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "evaluate",
                                            args, 2000);
        assert(out.str().find("control gate: would pass") != std::string::npos);
    }

    /* Only the enabled kind is enabled in the status view. */
    {
        std::ostringstream out;
        const std::vector<std::string_view> args;
        atperson::cli::run_outbound_command(out, policy_file, budget_file, control_file, "status",
                                            args, 2000);
        const std::string text = out.str();
        assert(text.find("post: enabled") != std::string::npos);
        assert(text.find("like: disabled") != std::string::npos);
    }
    std::printf("ok CLI surface\n");
}

} // namespace

int main() {
    test_kind_names_round_trip();
    test_default_policy_denies_every_kind();
    test_policy_round_trip();
    test_policy_rejects_bad_documents();
    test_missing_policy_file_is_default_deny();
    test_allow_then_window_exhaustion();
    test_min_interval_cooldown();
    test_duplicate_suppression();
    test_backwards_clock_is_conservative();
    test_restart_preserves_budgets();
    test_missing_and_corrupt_budget_is_fail_closed();
    test_concurrent_proposals_serialise_at_admission();
    test_records_are_bounded_and_pruned();
    test_unsupported_kind_is_denied();
    test_cli_surface();
    return 0;
}
