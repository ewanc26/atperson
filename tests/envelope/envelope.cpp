/* Standing authorization envelopes (#141): bounded operator pre-approval
 * that composes with the existing gates and never widens them. Offline:
 * no network, injected clock, real gate files. */
#include "control/envelope.hpp"
#include "control/state.hpp"
#include "outbound/action.hpp"
#include "outbound/actions.hpp"
#include "outbound/attempt.hpp"
#include "outbound/audit.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

using atperson::ActionBudget;
using atperson::AuthorizationEnvelope;
using atperson::ControlState;
using atperson::EnvelopeCoverage;
using atperson::EnvelopeEvidence;
using atperson::EnvelopeKindRule;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::OutboundAttemptPaths;
using atperson::OutboundPolicy;
using atperson::OutboundWriteResult;
using atperson::OutboundWriter;

constexpr std::int64_t NOW = 1'700'000'000;

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #condition \
                      << '\n';                                                  \
        }                                                                       \
    } while (false)

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-envelope-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void write_file(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

struct FakeWriter final : OutboundWriter {
    int put_calls = 0;
    std::string resolve_record_cid(const std::string &) override { return "bafycid"; }
    OutboundWriteResult put_record(const std::string &, const std::string &rkey,
                                   const std::string &) override {
        ++put_calls;
        OutboundWriteResult written;
        written.uri = "at://did:plc:self/app.bsky.feed.post/" + rkey;
        written.cid = "bafyrecord";
        return written;
    }
};

/* Policy allows posts: max 10/hour. */
OutboundPolicy permissive_post_policy() {
    OutboundPolicy policy;
    ActionBudget post;
    post.enabled = true;
    post.max_in_window = 10;
    post.window_seconds = 3600;
    atperson::budget_for(policy, OutboundActionKind::Post) = post;
    return policy;
}

/* Gate files: writes enabled, approval required, not paused, not dry-run. */
struct Gates {
    std::filesystem::path root;
    std::filesystem::path policy;
    std::filesystem::path budget;
    std::filesystem::path control;
    std::filesystem::path audit;
    std::filesystem::path journal;
    std::filesystem::path envelopes;

    Gates(const char *tag)
        : root(scratch_dir(tag)), policy(root / "policy.json"), budget(root / "budget.json"),
          control(root / "control.json"), audit(root / "audit.log"),
          journal(root / "journal.jsonl"), envelopes(root / "envelopes") {
        write_file(policy, atperson::serialise_outbound_policy(permissive_post_policy()));
        ControlState control_state;
        control_state.writes_enabled = true;
        control_state.dry_run = false;
        control_state.approval_required = true;
        atperson::save_control_state(control_state, control);
    }

    OutboundAttemptPaths attempt_paths() const {
        return OutboundAttemptPaths{policy, budget, control, audit, journal, envelopes};
    }
};

AuthorizationEnvelope simple_envelope(const std::string &id, std::uint32_t max_in_window) {
    AuthorizationEnvelope envelope;
    envelope.id = id;
    envelope.created_at = "2023-11-14T22:13:20Z";
    EnvelopeKindRule rule;
    rule.kind = OutboundActionKind::Post;
    rule.max_in_window = max_in_window;
    rule.window_seconds = 3600;
    envelope.kinds.push_back(rule);
    return envelope;
}

OutboundAction post_action(const std::string &text, const std::string &digest) {
    OutboundAction action;
    action.kind = OutboundActionKind::Post;
    action.text = text;
    action.rkey = "3lbjc2q2sc2g7";
    action.created_at = "2023-11-14T22:13:20Z";
    action.digest = digest;
    return action;
}

/* 1. A qualifying action is covered and executes without digest approval. */
void test_envelope_allows_qualifying_action() {
    Gates gates("allows");
    const OutboundPolicy policy = permissive_post_policy();
    atperson::save_authorization_envelope(simple_envelope("daily", 3), gates.envelopes);

    const auto envelope = atperson::load_authorization_envelope(gates.envelopes, "daily", policy);
    CHECK(envelope.has_value());

    const EnvelopeCoverage coverage = atperson::covers_action(
        *envelope, OutboundActionKind::Post, "hello world", EnvelopeEvidence{},
        atperson::OutboundKindUsage{}, NOW);
    CHECK(coverage.covered);
    CHECK(coverage.envelope_id == "daily");

    /* End to end: the attempt atom executes without any approved digest. */
    FakeWriter writer;
    const OutboundAction action = post_action("hello world", "0123456789abcdef");
    const auto result = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(result.outcome == atperson::OutboundExecutionOutcome::Executed);
    CHECK(writer.put_calls == 1);
    CHECK(result.authorization.envelope_id == "daily");

    /* The audit log records which envelope covered the write. */
    std::ifstream audit(gates.audit);
    std::string line;
    std::getline(audit, line);
    CHECK(line.find("\"envelope_id\":\"daily\"") != std::string::npos);
}

/* 2. An expired envelope does not cover: per-digest approval is required. */
void test_expired_envelope_falls_back() {
    Gates gates("expired");
    const OutboundPolicy policy = permissive_post_policy();
    AuthorizationEnvelope envelope = simple_envelope("stale", 3);
    envelope.expires_at = "2023-11-14T22:13:20Z"; /* NOW - 1 second. */
    atperson::save_authorization_envelope(envelope, gates.envelopes);

    const atperson::OutboundBudgetState budget;
    const EnvelopeCoverage coverage = atperson::find_covering_envelope(
        gates.envelopes, policy, OutboundActionKind::Post, "hello world", EnvelopeEvidence{},
        budget, NOW);
    CHECK(!coverage.covered);
    CHECK(coverage.reason == "expired");

    /* End to end: the unapproved action is denied with approval_required. */
    FakeWriter writer;
    const OutboundAction action = post_action("hello world", "0123456789abcdef");
    const auto result = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(result.outcome == atperson::OutboundExecutionOutcome::Denied);
    CHECK(result.reason_code == "approval_required");
    CHECK(writer.put_calls == 0);
}

/* 3. An envelope ceiling tighter than the policy wins. */
void test_tighter_ceiling_wins() {
    Gates gates("ceiling");
    const OutboundPolicy policy = permissive_post_policy();
    atperson::save_authorization_envelope(simple_envelope("tight", 2), gates.envelopes);

    /* Two actions already recorded in the budget: the envelope ceiling of
     * 2 is exhausted even though the policy allows 10. */
    atperson::OutboundBudgetState budget;
    atperson::OutboundKindUsage &usage =
        budget.usage[static_cast<std::size_t>(OutboundActionKind::Post)];
    usage.recent_actions = {NOW - 60, NOW - 30};

    const EnvelopeCoverage coverage = atperson::find_covering_envelope(
        gates.envelopes, policy, OutboundActionKind::Post, "hello world", EnvelopeEvidence{},
        budget, NOW);
    CHECK(!coverage.covered);
    CHECK(coverage.reason == "rate_ceiling");

    /* A document whose ceiling exceeds the policy is rejected at parse. */
    const std::string wide = atperson::serialise_authorization_envelope(
        simple_envelope("wide", 11));
    bool rejected = false;
    try {
        atperson::parse_authorization_envelope(wide, policy, "test");
    } catch (const atperson::EnvelopeError &) {
        rejected = true;
    }
    CHECK(rejected);
}

/* 4. Pause overrides everything, including a covering envelope. */
void test_pause_overrides_envelope() {
    Gates gates("paused");
    atperson::save_authorization_envelope(simple_envelope("daily", 3), gates.envelopes);

    ControlState paused_state;
    paused_state.paused = true;
    paused_state.writes_enabled = true;
    paused_state.dry_run = false;
    paused_state.approval_required = true;
    atperson::save_control_state(paused_state, gates.control);

    FakeWriter writer;
    const OutboundAction action = post_action("hello world", "0123456789abcdef");
    const auto result = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(result.outcome == atperson::OutboundExecutionOutcome::Denied);
    CHECK(result.reason_code == "paused");
    CHECK(writer.put_calls == 0);
}

/* 5. Revocation between attempts stops the next execution immediately. */
void test_revocation_stops_next_execution() {
    Gates gates("revoke");
    atperson::save_authorization_envelope(simple_envelope("daily", 3), gates.envelopes);

    FakeWriter writer;
    const OutboundAction action = post_action("hello world", "0123456789abcdef");
    const auto first = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(first.outcome == atperson::OutboundExecutionOutcome::Executed);

    atperson::revoke_authorization_envelope(gates.envelopes, "daily");

    const OutboundAction second_action = post_action("more text", "fedcba9876543210");
    const auto second = atperson::attempt_outbound_action(
        second_action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; },
        NOW + 10);
    CHECK(second.outcome == atperson::OutboundExecutionOutcome::Denied);
    CHECK(second.reason_code == "approval_required");
    CHECK(writer.put_calls == 1); /* only the pre-revocation write */
}

/* 6. Score floors: no evidence fails a configured floor (fail-closed);
 *    evidence below the floor fails; evidence above passes. */
void test_score_floors() {
    Gates gates("floors");
    const OutboundPolicy policy = permissive_post_policy();
    AuthorizationEnvelope envelope = simple_envelope("floored", 3);
    envelope.kinds[0].min_plan_score = 0.5;
    envelope.kinds[0].min_support_score = 0.2;
    atperson::save_authorization_envelope(envelope, gates.envelopes);

    const auto loaded = atperson::load_authorization_envelope(gates.envelopes, "floored", policy);
    const atperson::OutboundKindUsage usage;

    /* No evidence at all: fail-closed. */
    EnvelopeCoverage coverage = atperson::covers_action(
        *loaded, OutboundActionKind::Post, "hello", EnvelopeEvidence{}, usage, NOW);
    CHECK(!coverage.covered);
    CHECK(coverage.reason == "score_floor");

    /* Below the plan floor. */
    coverage = atperson::covers_action(*loaded, OutboundActionKind::Post, "hello",
                                       EnvelopeEvidence{0.3, 0.9}, usage, NOW);
    CHECK(!coverage.covered);
    CHECK(coverage.reason == "score_floor");

    /* Meets both floors. */
    coverage = atperson::covers_action(*loaded, OutboundActionKind::Post, "hello",
                                       EnvelopeEvidence{0.6, 0.25}, usage, NOW);
    CHECK(coverage.covered);
}

/* 7. Scope terms: out-of-scope text is not covered. */
void test_scope_restriction() {
    Gates gates("scope");
    const OutboundPolicy policy = permissive_post_policy();
    AuthorizationEnvelope envelope = simple_envelope("scoped", 3);
    envelope.scope_terms = {"moon", "wolf"};
    atperson::save_authorization_envelope(envelope, gates.envelopes);

    const auto loaded = atperson::load_authorization_envelope(gates.envelopes, "scoped", policy);
    const atperson::OutboundKindUsage usage;

    EnvelopeCoverage coverage = atperson::covers_action(*loaded, OutboundActionKind::Post,
                                                        "hello world", EnvelopeEvidence{}, usage,
                                                        NOW);
    CHECK(!coverage.covered);
    CHECK(coverage.reason == "scope");

    coverage = atperson::covers_action(*loaded, OutboundActionKind::Post,
                                       "the Wolf howls tonight", EnvelopeEvidence{}, usage, NOW);
    CHECK(coverage.covered);
}

/* 8. Round-trip and malformed documents. */
void test_document_round_trip() {
    Gates gates("roundtrip");
    const OutboundPolicy policy = permissive_post_policy();
    AuthorizationEnvelope envelope = simple_envelope("rt", 3);
    envelope.scope_terms = {"moon"};
    envelope.note = "test envelope";
    envelope.expires_at = "2030-01-01T00:00:00Z";

    const std::string json = atperson::serialise_authorization_envelope(envelope);
    const auto parsed = atperson::parse_authorization_envelope(json, policy, "round-trip");
    CHECK(parsed.id == "rt");
    CHECK(parsed.expires_at == "2030-01-01T00:00:00Z");
    CHECK(parsed.scope_terms == std::vector<std::string>{"moon"});
    CHECK(parsed.note == "test envelope");
    CHECK(parsed.kinds.size() == 1u);
    CHECK(parsed.kinds[0].max_in_window == 3u);

    /* Malformed documents are rejected, never guessed at. */
    bool rejected = false;
    try {
        atperson::parse_authorization_envelope("{\"format\":\"wrong\"}", policy, "bad");
    } catch (const atperson::EnvelopeError &) {
        rejected = true;
    }
    CHECK(rejected);

    /* An envelope naming a kind the policy does not enable is rejected. */
    AuthorizationEnvelope reply_only = simple_envelope("reply-only", 1);
    reply_only.kinds[0].kind = OutboundActionKind::Reply;
    rejected = false;
    try {
        atperson::parse_authorization_envelope(
            atperson::serialise_authorization_envelope(reply_only), policy, "bad-kind");
    } catch (const atperson::EnvelopeError &) {
        rejected = true;
    }
    CHECK(rejected);
}

/* 9. No envelopes at all: exactly the pre-#141 per-digest behaviour. */
void test_no_envelope_is_per_digest() {
    Gates gates("none");
    FakeWriter writer;
    const OutboundAction action = post_action("hello world", "0123456789abcdef");
    const auto result = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(result.outcome == atperson::OutboundExecutionOutcome::Denied);
    CHECK(result.reason_code == "approval_required");
    CHECK(writer.put_calls == 0);

    /* With the digest approved, it executes and the audit entry carries no
     * envelope id. */
    ControlState control_state;
    control_state.writes_enabled = true;
    control_state.dry_run = false;
    control_state.approval_required = true;
    control_state.approved_digests.push_back("0123456789abcdef");
    atperson::save_control_state(control_state, gates.control);
    const auto approved = atperson::attempt_outbound_action(
        action, gates.attempt_paths(), [&writer]() -> OutboundWriter & { return writer; }, NOW);
    CHECK(approved.outcome == atperson::OutboundExecutionOutcome::Executed);
    CHECK(approved.authorization.digest_approved);
    CHECK(approved.authorization.envelope_id.empty());
}

} // namespace

int main() {
    test_envelope_allows_qualifying_action();
    test_expired_envelope_falls_back();
    test_tighter_ceiling_wins();
    test_pause_overrides_envelope();
    test_revocation_stops_next_execution();
    test_score_floors();
    test_scope_restriction();
    test_document_round_trip();
    test_no_envelope_is_per_digest();
    if (failures == 0) {
        std::cout << "envelope: all tests passed\n";
        return 0;
    }
    std::cout << "envelope: " << failures << " failure(s)\n";
    return 1;
}
