/* Outbound execution (#25): the ordered gate chain, fail-closed refusal,
 * budget-on-confirmed-success, idempotent frozen rkey, reply CID resolution
 * and the credential-free audit log. Offline: the network is a fake writer. */
#include "outbound/execute.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/audit.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using atperson::ActionBudget;
using atperson::ControlState;
using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::OutboundAuditEntry;
using atperson::OutboundAuditOutcome;
using atperson::OutboundBudgetState;
using atperson::OutboundExecutionOutcome;
using atperson::OutboundExecutionResult;
using atperson::OutboundPolicy;
using atperson::OutboundWriter;
using atperson::OutboundWriteResult;
using atperson::OutboundWriterFactory;

constexpr std::int64_t NOW = 1'700'000'000;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-publish-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

OutboundPolicy enabled_policy(OutboundActionKind kind, const ActionBudget &budget) {
    OutboundPolicy policy;
    atperson::budget_for(policy, kind) = budget;
    return policy;
}

ActionBudget allow_all_budget() {
    ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 10;
    budget.window_seconds = 3600;
    budget.min_interval_seconds = 0;
    budget.duplicate_cooldown_seconds = 0;
    return budget;
}

ControlState open_control() {
    ControlState control;
    control.paused = false;
    control.writes_enabled = true;
    control.dry_run = false;
    control.approval_required = false;
    return control;
}

OutboundAction post_action(const char *text = "hello world",
                           const char *digest = "0123456789abcdef") {
    OutboundAction action;
    action.kind = OutboundActionKind::Post;
    action.text = text;
    action.rkey = "3kabc";
    action.created_at = "2026-09-17T00:00:00Z";
    action.digest = digest;
    return action;
}

OutboundAction reply_action() {
    OutboundAction action;
    action.kind = OutboundActionKind::Reply;
    action.text = "a reply";
    action.rkey = "3kreply";
    action.created_at = "2026-09-17T00:00:01Z";
    action.digest = "fedcba9876543210";
    action.reply_root = "at://did:plc:root/app.bsky.feed.post/3kroot";
    action.reply_parent = "at://did:plc:parent/app.bsky.feed.post/3kparent";
    return action;
}

OutboundAction like_action() {
    OutboundAction action;
    action.kind = OutboundActionKind::Like;
    action.rkey = "3klike";
    action.created_at = "2026-09-17T00:00:02Z";
    action.digest = "abcdef0123456789";
    action.subject = "at://did:plc:author/app.bsky.feed.post/3ksubject";
    return action;
}

OutboundAction follow_action() {
    OutboundAction action;
    action.kind = OutboundActionKind::Follow;
    action.rkey = "3kfollow";
    action.created_at = "2026-09-17T00:00:03Z";
    action.digest = "13579bdf02468ace";
    action.subject = "did:plc:followed";
    return action;
}

struct FakeWriter final : OutboundWriter {
    int resolve_calls = 0;
    int put_calls = 0;
    bool fail_resolve = false;
    bool fail_put = false;
    std::vector<std::string> resolved;
    std::string collection;
    std::string rkey;
    std::string record_json;

    std::string resolve_record_cid(const std::string &at_uri) override {
        ++resolve_calls;
        resolved.push_back(at_uri);
        if (fail_resolve) {
            throw std::runtime_error("resolve failed");
        }
        return "bafycid(" + at_uri + ")";
    }

    OutboundWriteResult put_record(const std::string &collection_arg, const std::string &rkey_arg,
                                   const std::string &record_json_arg) override {
        ++put_calls;
        collection = collection_arg;
        rkey = rkey_arg;
        record_json = record_json_arg;
        if (fail_put) {
            throw std::runtime_error("put failed");
        }
        OutboundWriteResult written;
        written.uri = "at://did:plc:self/" + collection_arg + "/" + rkey_arg;
        written.cid = "bafyrecord";
        return written;
    }
};

OutboundExecutionResult run(const ControlState &control, const OutboundPolicy &policy,
                            OutboundBudgetState &budget, const OutboundAction &action,
                            OutboundWriterFactory factory) {
    return atperson::execute_outbound_action(control, policy, budget, action, factory, NOW);
}

void test_action_document_round_trip() {
    const OutboundAction original = reply_action();
    const std::string json = atperson::serialise_outbound_action(original);
    const OutboundAction parsed = atperson::parse_outbound_action(json, "test");
    assert(parsed.kind == OutboundActionKind::Reply);
    assert(parsed.text == original.text);
    assert(parsed.rkey == original.rkey);
    assert(parsed.created_at == original.created_at);
    assert(parsed.digest == original.digest);
    assert(parsed.reply_root == original.reply_root);
    assert(parsed.reply_parent == original.reply_parent);

    const auto proposal = atperson::outbound_action_proposal(original);
    assert(proposal.kind == OutboundActionKind::Reply);
    assert(proposal.target == original.reply_parent);
    assert(proposal.action_digest == original.digest);

    /* #152: like and follow round-trip through the same document shape,
     * and their proposals target the subject. */
    const OutboundAction like = like_action();
    const OutboundAction parsed_like =
        atperson::parse_outbound_action(atperson::serialise_outbound_action(like), "test");
    assert(parsed_like.kind == OutboundActionKind::Like);
    assert(parsed_like.subject == like.subject);
    assert(parsed_like.text.empty());
    const auto like_proposal = atperson::outbound_action_proposal(parsed_like);
    assert(like_proposal.target == like.subject);

    const OutboundAction follow = follow_action();
    const OutboundAction parsed_follow =
        atperson::parse_outbound_action(atperson::serialise_outbound_action(follow), "test");
    assert(parsed_follow.kind == OutboundActionKind::Follow);
    assert(parsed_follow.subject == follow.subject);
    const auto follow_proposal = atperson::outbound_action_proposal(parsed_follow);
    assert(follow_proposal.target == follow.subject);
    std::printf("ok action document round trip\n");
}

void test_action_document_rejects_bad_input() {
    const auto reject = [](const char *json) {
        bool threw = false;
        try {
            static_cast<void>(atperson::parse_outbound_action(json, "test"));
        } catch (const atperson::OutboundActionError &) {
            threw = true;
        }
        assert(threw);
    };

    reject(
        R"({"format":"other","version":1,"kind":"post","text":"x","rkey":"r","created_at":"t","digest":"d"})");
    reject(
        R"({"format":"atperson-outbound-action","version":2,"kind":"post","text":"x","rkey":"r","created_at":"t","digest":"d"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"like","text":"x","rkey":"r","created_at":"t","digest":"d"})");
    /* #152: a like without a subject, and a like with text. */
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"like","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"repost","text":"x","subject":"at://did:plc:a/app.bsky.feed.post/1","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"follow","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"x","subject":"did:plc:a","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    /* Unsupported kinds stay closed. */
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"unfollow","subject":"did:plc:a","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"moderation","text":"x","rkey":"r","created_at":"t","digest":"0123456789abcdef"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"nsfw","text":"x","rkey":"r","created_at":"t","digest":"d"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"","rkey":"r","created_at":"t","digest":"d"})");
    /* A digest that is not the 16-hex #22 control digest can never be approved. */
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"x","rkey":"r","created_at":"t","digest":"sha256:a"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"x","rkey":"r","created_at":"t","digest":"0123456789ABCDEF"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"x","rkey":"r","created_at":"t"})");
    /* A reply without context, and a post that claims reply context. */
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"reply","text":"x","rkey":"r","created_at":"t","digest":"d"})");
    reject(
        R"({"format":"atperson-outbound-action","version":1,"kind":"post","text":"x","rkey":"r","created_at":"t","digest":"d","reply":{"root":"a","parent":"b"}})");
    std::printf("ok action document rejects bad input\n");
}

void test_record_json_shape() {
    const OutboundAction post = post_action();
    const std::string post_json = atperson::build_outbound_record_json(post, "", "", "");
    assert(post_json.find("app.bsky.feed.post") != std::string::npos);
    assert(post_json.find("hello world") != std::string::npos);
    assert(post_json.find("reply") == std::string::npos);

    const OutboundAction reply = reply_action();
    const std::string reply_json =
        atperson::build_outbound_record_json(reply, "bafyroot", "bafyparent", "");
    assert(reply_json.find("com.atproto.repo.strongRef") != std::string::npos);
    assert(reply_json.find("bafyroot") != std::string::npos);
    assert(reply_json.find("bafyparent") != std::string::npos);

    bool threw = false;
    try {
        static_cast<void>(atperson::build_outbound_record_json(reply, "", "", ""));
    } catch (const atperson::OutboundActionError &) {
        threw = true;
    }
    assert(threw);

    /* #152: a like/repost record is a strongRef to its subject; a follow
     * record names the subject DID directly. No text in either. */
    const OutboundAction like = like_action();
    const std::string like_json =
        atperson::build_outbound_record_json(like, "", "", "bafysubject");
    assert(like_json.find("app.bsky.feed.like") != std::string::npos);
    assert(like_json.find("com.atproto.repo.strongRef") != std::string::npos);
    assert(like_json.find("bafysubject") != std::string::npos);
    assert(like_json.find("\"text\"") == std::string::npos);
    bool like_threw = false;
    try {
        static_cast<void>(atperson::build_outbound_record_json(like, "", "", ""));
    } catch (const atperson::OutboundActionError &) {
        like_threw = true;
    }
    assert(like_threw);

    const OutboundAction follow = follow_action();
    const std::string follow_json =
        atperson::build_outbound_record_json(follow, "", "", "");
    assert(follow_json.find("app.bsky.graph.follow") != std::string::npos);
    assert(follow_json.find("did:plc:followed") != std::string::npos);
    assert(follow_json.find("com.atproto.repo.strongRef") == std::string::npos);
    std::printf("ok record json shape\n");
}

void test_paused_refuses_before_writer() {
    ControlState control = open_control();
    control.paused = true;
    FakeWriter writer;
    OutboundBudgetState budget;
    int factory_calls = 0;
    const auto factory = [&](void) -> OutboundWriter & {
        ++factory_calls;
        return writer;
    };
    const auto result = run(control, enabled_policy(OutboundActionKind::Post, allow_all_budget()),
                            budget, post_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::Denied);
    assert(result.reason_code == "paused");
    assert(factory_calls == 0);
    assert(writer.put_calls == 0);
    assert(!result.budget_recorded);
    std::printf("ok paused refuses before writer\n");
}

void test_policy_deny_refuses_before_writer() {
    FakeWriter writer;
    OutboundBudgetState budget;
    int factory_calls = 0;
    const auto factory = [&](void) -> OutboundWriter & {
        ++factory_calls;
        return writer;
    };
    const OutboundPolicy policy; /* everything default-deny. */
    const auto result = run(open_control(), policy, budget, post_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::Denied);
    assert(result.reason_code == "kind_disabled");
    assert(factory_calls == 0);
    std::printf("ok policy deny refuses before writer\n");
}

void test_dry_run_allows_policy_but_writes_nothing() {
    ControlState control = open_control();
    control.dry_run = true;
    FakeWriter writer;
    OutboundBudgetState budget;
    int factory_calls = 0;
    const auto factory = [&](void) -> OutboundWriter & {
        ++factory_calls;
        return writer;
    };
    const auto result = run(control, enabled_policy(OutboundActionKind::Post, allow_all_budget()),
                            budget, post_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::DryRun);
    assert(result.reason_code == "dry_run");
    assert(factory_calls == 0);
    assert(writer.put_calls == 0);
    assert(!result.budget_recorded);
    std::printf("ok dry run writes nothing\n");
}

void test_writes_disabled_refuses() {
    ControlState control = open_control();
    control.writes_enabled = false;
    FakeWriter writer;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const auto result = run(control, enabled_policy(OutboundActionKind::Post, allow_all_budget()),
                            budget, post_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::Denied);
    assert(result.reason_code == "writes_disabled");
    assert(writer.put_calls == 0);
    std::printf("ok writes-disabled refuses\n");
}

void test_approval_gate_refuses_unapproved_digest() {
    ControlState control = open_control();
    control.approval_required = true; /* nothing approved. */
    FakeWriter writer;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const auto result = run(control, enabled_policy(OutboundActionKind::Post, allow_all_budget()),
                            budget, post_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::Denied);
    assert(result.reason_code == "approval_required");
    assert(writer.put_calls == 0);

    /* Approving the exact digest opens the gate. */
    control.approved_digests.push_back("0123456789abcdef");
    const auto approved = run(control, enabled_policy(OutboundActionKind::Post, allow_all_budget()),
                              budget, post_action(), factory);
    assert(approved.outcome == OutboundExecutionOutcome::Executed);
    assert(writer.put_calls == 1);
    std::printf("ok approval gate refuses unapproved digest\n");
}

void test_execute_writes_exact_text_and_frozen_rkey() {
    FakeWriter writer;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const auto result =
        run(open_control(), enabled_policy(OutboundActionKind::Post, allow_all_budget()), budget,
            post_action("the exact approved text"), factory);
    assert(result.outcome == OutboundExecutionOutcome::Executed);
    assert(result.reason_code == "allow");
    assert(result.budget_recorded);
    assert(result.written.uri == "at://did:plc:self/app.bsky.feed.post/3kabc");
    assert(writer.put_calls == 1);
    assert(writer.collection == "app.bsky.feed.post");
    assert(writer.rkey == "3kabc");
    assert(writer.record_json.find("the exact approved text") != std::string::npos);
    std::printf("ok execute writes exact text and frozen rkey\n");
}

void test_execute_like_repost_follow_kinds() {
    /* #152: like/repost/follow pass the same gate chain and reach the write
     * under their own collections. A like resolves the subject CID first. */
    {
        FakeWriter writer;
        OutboundBudgetState budget;
        const auto factory = [&](void) -> OutboundWriter & { return writer; };
        const auto result = run(open_control(),
                                enabled_policy(OutboundActionKind::Like, allow_all_budget()),
                                budget, like_action(), factory);
        assert(result.outcome == OutboundExecutionOutcome::Executed);
        assert(result.reason_code == "allow");
        assert(result.budget_recorded);
        assert(writer.put_calls == 1);
        assert(writer.collection == "app.bsky.feed.like");
        assert(writer.rkey == "3klike");
        assert(writer.resolve_calls == 1);
        assert(writer.resolved.at(0) == like_action().subject);
        assert(writer.record_json.find("bafycid(") != std::string::npos);
        assert(result.written.uri == "at://did:plc:self/app.bsky.feed.like/3klike");
    }
    {
        FakeWriter writer;
        OutboundBudgetState budget;
        const auto factory = [&](void) -> OutboundWriter & { return writer; };
        OutboundAction repost = like_action();
        repost.kind = OutboundActionKind::Repost;
        repost.rkey = "3krepost";
        const auto result = run(open_control(),
                                enabled_policy(OutboundActionKind::Repost, allow_all_budget()),
                                budget, repost, factory);
        assert(result.outcome == OutboundExecutionOutcome::Executed);
        assert(writer.collection == "app.bsky.feed.repost");
        assert(writer.record_json.find("app.bsky.feed.repost") != std::string::npos);
    }
    {
        /* A follow resolves no CIDs: the record names the subject DID. */
        FakeWriter writer;
        OutboundBudgetState budget;
        const auto factory = [&](void) -> OutboundWriter & { return writer; };
        const auto result = run(open_control(),
                                enabled_policy(OutboundActionKind::Follow, allow_all_budget()),
                                budget, follow_action(), factory);
        assert(result.outcome == OutboundExecutionOutcome::Executed);
        assert(writer.collection == "app.bsky.graph.follow");
        assert(writer.resolve_calls == 0);
        assert(writer.record_json.find("did:plc:followed") != std::string::npos);
    }
    {
        /* Default-deny: a like with no policy entry is refused, not written. */
        FakeWriter writer;
        OutboundBudgetState budget;
        const auto factory = [&](void) -> OutboundWriter & { return writer; };
        const auto result =
            run(open_control(), atperson::OutboundPolicy{}, budget, like_action(), factory);
        assert(result.outcome == OutboundExecutionOutcome::Denied);
        assert(result.reason_code == "kind_disabled");
        assert(writer.put_calls == 0);
    }
    std::printf("ok execute like repost follow kinds\n");
}

void test_budget_recorded_only_on_confirmed_success() {
    FakeWriter writer;
    writer.fail_put = true;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const OutboundPolicy policy = enabled_policy(OutboundActionKind::Post, allow_all_budget());

    const auto failed = run(open_control(), policy, budget, post_action(), factory);
    assert(failed.outcome == OutboundExecutionOutcome::Failed);
    assert(failed.reason_code == "write_failed");
    assert(!failed.budget_recorded);
    assert(writer.put_calls == 1);
    assert(atperson::count_outbound_actions_in_window(budget, OutboundActionKind::Post, 3600,
                                                      NOW) == 0);

    /* The ambiguous failure did not consume budget or suppress duplicates, so
     * the same frozen rkey can be retried and now succeeds. */
    writer.fail_put = false;
    const auto retried = run(open_control(), policy, budget, post_action(), factory);
    assert(retried.outcome == OutboundExecutionOutcome::Executed);
    assert(retried.budget_recorded);
    assert(writer.rkey == "3kabc");
    assert(atperson::count_outbound_actions_in_window(budget, OutboundActionKind::Post, 3600,
                                                      NOW) == 1);
    std::printf("ok budget recorded only on confirmed success\n");
}

void test_duplicate_cooldown_defers_after_success() {
    ActionBudget budget_config = allow_all_budget();
    budget_config.duplicate_cooldown_seconds = 3600;
    const OutboundPolicy policy = enabled_policy(OutboundActionKind::Post, budget_config);
    FakeWriter writer;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };

    const auto first = run(open_control(), policy, budget, post_action(), factory);
    assert(first.outcome == OutboundExecutionOutcome::Executed);
    const auto second = run(open_control(), policy, budget, post_action(), factory);
    assert(second.outcome == OutboundExecutionOutcome::Deferred);
    assert(second.reason_code == "duplicate_suppressed");
    assert(writer.put_calls == 1);
    std::printf("ok duplicate cooldown defers after success\n");
}

void test_reply_resolves_cids_and_builds_strong_refs() {
    FakeWriter writer;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const OutboundAction reply = reply_action();
    const auto result =
        run(open_control(), enabled_policy(OutboundActionKind::Reply, allow_all_budget()), budget,
            reply, factory);
    assert(result.outcome == OutboundExecutionOutcome::Executed);
    assert(writer.resolve_calls == 2);
    assert(writer.resolved.size() == 2);
    assert(writer.resolved[0] == reply.reply_root);
    assert(writer.resolved[1] == reply.reply_parent);
    assert(writer.record_json.find("bafycid(" + reply.reply_root + ")") != std::string::npos);
    assert(writer.record_json.find("bafycid(" + reply.reply_parent + ")") != std::string::npos);
    std::printf("ok reply resolves cids and builds strong refs\n");
}

void test_resolve_failure_is_failed_and_unbudgeted() {
    FakeWriter writer;
    writer.fail_resolve = true;
    OutboundBudgetState budget;
    const auto factory = [&](void) -> OutboundWriter & { return writer; };
    const auto result =
        run(open_control(), enabled_policy(OutboundActionKind::Reply, allow_all_budget()), budget,
            reply_action(), factory);
    assert(result.outcome == OutboundExecutionOutcome::Failed);
    assert(result.reason_code == "write_failed");
    assert(writer.put_calls == 0);
    assert(!result.budget_recorded);
    std::printf("ok resolve failure is failed and unbudgeted\n");
}

void test_audit_log_is_append_only() {
    const auto dir = scratch_dir("audit");
    const auto path = dir / "outbound-audit.jsonl";

    OutboundAuditEntry first;
    first.at = "2026-09-17T00:00:00Z";
    first.kind = "post";
    first.rkey = "3kabc";
    first.digest = "0123456789abcdef";
    first.outcome = OutboundAuditOutcome::Executed;
    first.reason = "allow";
    first.detail = "action written";
    first.uri = "at://did:plc:self/app.bsky.feed.post/3kabc";
    first.cid = "bafyrecord";
    atperson::append_outbound_audit(path, first);

    OutboundAuditEntry second;
    second.at = "2026-09-17T00:00:01Z";
    second.kind = "post";
    second.rkey = "3kdef";
    second.digest = "0011223344556677";
    second.outcome = OutboundAuditOutcome::Denied;
    second.reason = "kind_disabled";
    atperson::append_outbound_audit(path, second);

    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    assert(lines.size() == 2);
    assert(lines[0].find("\"outcome\":\"executed\"") != std::string::npos);
    assert(lines[0].find("bafyrecord") != std::string::npos);
    assert(lines[1].find("\"outcome\":\"denied\"") != std::string::npos);
    assert(lines[1].find("kind_disabled") != std::string::npos);
    std::printf("ok audit log is append only\n");
}

void test_action_file_load() {
    const auto dir = scratch_dir("load");
    const auto path = dir / "action.json";
    {
        std::ofstream file(path);
        file << atperson::serialise_outbound_action(post_action("loaded text"));
    }
    const OutboundAction action = atperson::load_outbound_action(path);
    assert(action.text == "loaded text");
    assert(action.kind == OutboundActionKind::Post);

    bool threw = false;
    try {
        static_cast<void>(atperson::load_outbound_action(dir / "missing.json"));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok action file load\n");
}

/* #27: the journal records every publish attempt with the rkey as the
 * stable action id, and the executed record's at-URI is what later event
 * linkage keys on. */
void test_journal_records_every_attempt() {
    const auto dir = scratch_dir("journal");
    const auto path = dir / "action-journal.jsonl";

    JournalAction executed;
    executed.id = "3kabc";
    executed.kind = "post";
    executed.text = "the exact approved text";
    executed.digest = "0123456789abcdef";
    executed.outcome = JournalActionOutcome::Executed;
    executed.reason = "allow";
    executed.uri = "at://did:plc:self/app.bsky.feed.post/3kabc";
    executed.cid = "bafyreiexample";
    executed.at = "2026-09-17T00:00:00Z";
    atperson::append_journal_action(path, executed);

    JournalAction denied = executed;
    denied.id = "3kdef";
    denied.outcome = JournalActionOutcome::Denied;
    denied.reason = "approval_required";
    denied.uri.clear();
    denied.cid.clear();
    atperson::append_journal_action(path, denied);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 2u);
    assert(!journal.repaired_torn_tail);

    /* Executed and denied attempts are distinguishable, and the executed
     * action is found by its result URI for event linkage. */
    const JournalAction *found =
        atperson::journal_find_action_by_uri(journal, "at://did:plc:self/app.bsky.feed.post/3kabc");
    assert(found != nullptr);
    assert(found->id == "3kabc");
    assert(found->outcome == JournalActionOutcome::Executed);
    assert(atperson::journal_find_action_by_uri(
               journal, "at://did:plc:self/app.bsky.feed.post/3kdef") == nullptr);
    std::printf("ok journal records every attempt\n");
}

} // namespace

int main() {
    test_action_document_round_trip();
    test_action_document_rejects_bad_input();
    test_record_json_shape();
    test_paused_refuses_before_writer();
    test_policy_deny_refuses_before_writer();
    test_dry_run_allows_policy_but_writes_nothing();
    test_writes_disabled_refuses();
    test_approval_gate_refuses_unapproved_digest();
    test_execute_writes_exact_text_and_frozen_rkey();
    test_execute_like_repost_follow_kinds();
    test_budget_recorded_only_on_confirmed_success();
    test_duplicate_cooldown_defers_after_success();
    test_reply_resolves_cids_and_builds_strong_refs();
    test_resolve_failure_is_failed_and_unbudgeted();
    test_audit_log_is_append_only();
    test_journal_records_every_attempt();
    test_action_file_load();
    std::printf("all outbound execution tests passed\n");
    return 0;
}
