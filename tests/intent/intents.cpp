/* Pending social intents (#150): configuration bounds, derived open/expired/
 * closed states, the idempotent expiry sweep, record_pending_intent
 * (open/continue/duplicate/cap), the continuation trigger over journal
 * events, and the intent-aware expectation hand-off. Offline, no network. */

#include "intent/config.hpp"
#include "intent/mutate.hpp"
#include "intent/state.hpp"
#include "intent/sweep.hpp"
#include "journal/resolve.hpp"
#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/execute.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using atperson::IntentConfig;
using atperson::IntentMutation;
using atperson::IntentState;
using atperson::IntentSweepReport;
using atperson::JournalContents;
using atperson::JournalExpectationState;
using atperson::JournalEvent;
using atperson::JournalIntent;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::OutboundExecutionOutcome;
using atperson::OutboundExecutionResult;
using atperson::OutboundWriteResult;
using atperson::derive_expectation_state;
using atperson::derive_intent_state;
using atperson::intent_config_from_environment;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-intent-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/* Epochs from the #149 fixture family: 2026-09-17T10:00:00Z etc. */
constexpr std::int64_t kOpenAt = 1789639200;   /* 2026-09-17T10:00:00Z */
constexpr std::int64_t kNextDay = 1789725600;  /* 2026-09-18T10:00:00Z */
const std::string kThreadUri = "at://did:plc:example/app.bsky.feed.post/3lzc7thread";
const std::string kReplyUri = "at://did:plc:other/app.bsky.feed.post/3lzc7reply1";

OutboundAction sample_action() {
    OutboundAction action;
    action.kind = OutboundActionKind::Post;
    action.text = "what do you make of the moon?";
    action.rkey = "3lzc7a2pfxn2c";
    action.created_at = "2026-09-17T10:00:00Z";
    action.digest = "0123456789abcdef";
    return action;
}

OutboundExecutionResult executed_result(const std::string &uri) {
    OutboundExecutionResult result;
    result.outcome = OutboundExecutionOutcome::Executed;
    result.written = OutboundWriteResult{uri, "bafyreiabc123"};
    return result;
}

void test_config_defaults_and_clamps() {
    const IntentConfig from_env = intent_config_from_environment();
    assert(!from_env.enabled);
    std::printf("ok config defaults read from a clean environment\n");
}

void test_config_env_overrides() {
    const char *enabled = std::getenv("ATPERSON_INTENTS");
    const char *active = std::getenv("ATPERSON_INTENT_MAX_ACTIVE");
    const char *continuations = std::getenv("ATPERSON_INTENT_MAX_CONTINUATIONS");
    const char *window = std::getenv("ATPERSON_INTENT_WINDOW_DAYS");
    setenv("ATPERSON_INTENTS", "1", 1);
    setenv("ATPERSON_INTENT_MAX_ACTIVE", "5", 1);
    setenv("ATPERSON_INTENT_MAX_CONTINUATIONS", "7", 1);
    setenv("ATPERSON_INTENT_WINDOW_DAYS", "4", 1);
    const IntentConfig config = intent_config_from_environment();
    assert(config.enabled);
    assert(config.max_active == 5u);
    assert(config.max_continuations == 7u);
    assert(config.window_seconds == 4 * 24 * 3600);
    if (enabled == nullptr) unsetenv("ATPERSON_INTENTS");
    else setenv("ATPERSON_INTENTS", enabled, 1);
    if (active == nullptr) unsetenv("ATPERSON_INTENT_MAX_ACTIVE");
    else setenv("ATPERSON_INTENT_MAX_ACTIVE", active, 1);
    if (continuations == nullptr) unsetenv("ATPERSON_INTENT_MAX_CONTINUATIONS");
    else setenv("ATPERSON_INTENT_MAX_CONTINUATIONS", continuations, 1);
    if (window == nullptr) unsetenv("ATPERSON_INTENT_WINDOW_DAYS");
    else setenv("ATPERSON_INTENT_WINDOW_DAYS", window, 1);
    std::printf("ok config env overrides\n");
}

void test_config_bounds_are_clamped() {
    setenv("ATPERSON_INTENTS", "1", 1);
    setenv("ATPERSON_INTENT_MAX_ACTIVE", "999999", 1);
    setenv("ATPERSON_INTENT_MAX_CONTINUATIONS", "999999", 1);
    setenv("ATPERSON_INTENT_WINDOW_DAYS", "99999", 1);
    const IntentConfig config = intent_config_from_environment();
    assert(config.max_active == atperson::kMaxActiveIntents);
    assert(config.max_continuations == atperson::kMaxIntentContinuations);
    assert(config.window_seconds == atperson::kMaxIntentWindowSeconds);
    unsetenv("ATPERSON_INTENTS");
    unsetenv("ATPERSON_INTENT_MAX_ACTIVE");
    unsetenv("ATPERSON_INTENT_MAX_CONTINUATIONS");
    unsetenv("ATPERSON_INTENT_WINDOW_DAYS");
    std::printf("ok config bounds clamped\n");
}

void test_derive_intent_state() {
    JournalIntent intent;
    intent.id = kThreadUri;
    intent.actions = {"first"};
    intent.responder = "anyone";
    intent.expires_at_epoch = 1789812000; /* open + 2 days */
    intent.max_continuations = 3u;
    intent.state = IntentState::Open;

    /* Inside the window, budget unused: open. */
    assert(derive_intent_state(intent, kNextDay) == IntentState::Open);
    assert(derive_intent_state(intent, 1789812000) == IntentState::Open);
    /* Past the expiry instant: expired (strictly beyond). */
    assert(derive_intent_state(intent, 1789812001) == IntentState::Expired);

    /* The continuation budget closes the conversation inside its window. */
    intent.actions = {"first", "second", "third", "fourth"};
    assert(derive_intent_state(intent, kNextDay) == IntentState::Closed);

    /* A recorded terminal state never reopens. */
    intent.state = IntentState::Expired;
    intent.actions = {"first"};
    assert(derive_intent_state(intent, kNextDay) == IntentState::Expired);
    intent.state = IntentState::Closed;
    assert(derive_intent_state(intent, kNextDay) == IntentState::Closed);
    std::printf("ok derive_intent_state\n");
}

void test_latest_and_active_intents() {
    const auto root = scratch_dir("active");
    const auto path = root / "action-journal.jsonl";

    JournalIntent open_a;
    open_a.id = kThreadUri;
    open_a.actions = {"a1"};
    open_a.responder = "anyone";
    open_a.expires_at_epoch = 1790330400; /* far future */
    open_a.max_continuations = 3u;
    open_a.state = IntentState::Open;
    open_a.at_epoch = 1789639200u;
    open_a.at = "2026-09-17T10:00:00Z";
    atperson::append_journal_intent(path, open_a);

    JournalIntent open_b = open_a;
    open_b.id = "at://did:plc:example/app.bsky.feed.post/threadb";
    open_b.actions = {"b1"};
    atperson::append_journal_intent(path, open_b);

    /* A closed intent is not active. */
    JournalIntent done = open_a;
    done.id = "at://did:plc:example/app.bsky.feed.post/threadc";
    done.actions = {"c1", "c2", "c3", "c4"};
    atperson::append_journal_intent(path, done);

    const JournalContents journal = atperson::load_journal(path);
    assert(atperson::latest_intent(journal, kThreadUri) != nullptr);
    assert(atperson::latest_intent(journal, "at://x/y/z") == nullptr);
    assert(atperson::active_intent_count(journal, kNextDay) == 2u);
    const auto active = atperson::active_intents(journal, kNextDay);
    assert(active.size() == 2u);

    /* A continuation appends the same id; the latest entry is authoritative
     * and the count stays bounded per conversation. */
    JournalIntent continuation = open_a;
    continuation.actions = {"a1", "a2"};
    atperson::append_journal_intent(path, continuation);
    const JournalContents reloaded = atperson::load_journal(path);
    assert(atperson::active_intent_count(reloaded, kNextDay) == 2u);
    const JournalIntent *latest = atperson::latest_intent(reloaded, kThreadUri);
    assert(latest != nullptr && latest->actions.size() == 2u);
    std::printf("ok latest and active intents\n");
}

void test_continuation_trigger() {
    const auto root = scratch_dir("cont");
    const auto path = root / "action-journal.jsonl";

    JournalEvent event;
    event.action_id = "a1";
    event.event_uri = kReplyUri;
    event.author_did = "did:plc:other";
    event.via = "parent";
    event.at = "2026-09-17T12:00:00Z";
    atperson::append_journal_event(path, event);

    JournalIntent open;
    open.id = kThreadUri;
    open.actions = {"a1"};
    open.responder = "anyone";
    open.expires_at_epoch = 1790330400;
    open.max_continuations = 3u;
    open.state = IntentState::Open;
    open.at_epoch = 1789639200u;
    open.at = "2026-09-17T10:00:00Z";
    atperson::append_journal_intent(path, open);

    const JournalContents journal = atperson::load_journal(path);
    const JournalIntent *match = atperson::continuation_intent(
        journal, kReplyUri, "did:plc:other", kNextDay);
    assert(match != nullptr);
    assert(match->id == kThreadUri);

    /* A specific responder only matches that author. */
    JournalContents specific = journal;
    specific.intents[0].responder = "did:plc:other";
    assert(atperson::continuation_intent(specific, kReplyUri, "did:plc:other", kNextDay) != nullptr);
    assert(atperson::continuation_intent(specific, kReplyUri, "did:plc:stranger", kNextDay) ==
           nullptr);

    /* An observation that is not a journal event never matches. */
    assert(atperson::continuation_intent(journal, "at://did:plc:other/x/y", "did:plc:other",
                                         kNextDay) == nullptr);

    /* An action from outside the conversation never matches. */
    JournalEvent foreign = event;
    foreign.action_id = "z9";
    foreign.event_uri = "at://did:plc:other/app.bsky.feed.post/foreign";
    atperson::append_journal_event(path, foreign);
    const JournalContents with_foreign = atperson::load_journal(path);
    assert(atperson::continuation_intent(with_foreign, foreign.event_uri, "did:plc:other",
                                         kNextDay) == nullptr);

    /* An expired intent never triggers a continuation. */
    JournalIntent expired = open;
    expired.expires_at_epoch = 1789639200; /* already past at kNextDay */
    atperson::append_journal_intent(path, expired);
    const JournalContents with_expired = atperson::load_journal(path);
    assert(atperson::continuation_intent(with_expired, kReplyUri, "did:plc:other", kNextDay) ==
           nullptr);
    std::printf("ok continuation trigger\n");
}

void test_record_pending_intent() {
    const auto root = scratch_dir("mutate");
    const auto path = root / "action-journal.jsonl";

    IntentConfig config;
    config.enabled = true;
    config.max_active = 1u;
    config.max_continuations = 3u;
    config.window_seconds = 2 * 24 * 3600;

    /* Feature disabled: nothing is tracked. */
    IntentConfig inert;
    const OutboundAction first = sample_action();
    assert(atperson::record_pending_intent(path, first, executed_result(kThreadUri), inert,
                                           kOpenAt, "2026-09-17T10:00:00Z") ==
           IntentMutation::NotTracked);

    /* An executed original post opens an intent keyed to its record URI. */
    assert(atperson::record_pending_intent(path, first, executed_result(kThreadUri), config,
                                           kOpenAt, "2026-09-17T10:00:00Z") ==
           IntentMutation::Opened);

    /* The cap is exhausted: a second conversation is refused, not silently
     * dropped. */
    OutboundAction second = sample_action();
    second.rkey = "3lzc7a2pfxn3c";
    assert(atperson::record_pending_intent(path, second,
                                           executed_result("at://did:plc:example/app.bsky.feed.post/threadb"),
                                           config, kOpenAt, "2026-09-17T10:00:00Z") ==
           IntentMutation::CapReached);

    /* An executed reply continuing the open thread appends the action. */
    OutboundAction reply = sample_action();
    reply.kind = OutboundActionKind::Reply;
    reply.reply_root = kThreadUri;
    reply.reply_parent = kReplyUri;
    reply.rkey = "3lzc7a2pfxn4c";
    assert(atperson::record_pending_intent(path, reply, executed_result(kReplyUri), config,
                                           kNextDay, "2026-09-18T10:00:00Z") ==
           IntentMutation::Continued);

    /* The same action id is never recorded twice in a conversation. */
    OutboundAction duplicate = reply;
    assert(atperson::record_pending_intent(path, duplicate, executed_result(kReplyUri), config,
                                           kNextDay, "2026-09-18T10:00:00Z") ==
           IntentMutation::Duplicate);

    /* A non-executed result tracks nothing. */
    OutboundExecutionResult denied;
    denied.outcome = OutboundExecutionOutcome::Denied;
    assert(atperson::record_pending_intent(path, reply, denied, config, kNextDay,
                                           "2026-09-18T10:00:00Z") == IntentMutation::NotTracked);

    /* The journal carries the reconstructed conversation: one open intent,
     * capped, the continuation, and no duplicates. */
    const JournalContents journal = atperson::load_journal(path);
    assert(journal.intents.size() == 2u);
    const JournalIntent &opened = journal.intents[0];
    assert(opened.id == kThreadUri);
    assert(opened.state == IntentState::Open);
    assert(opened.actions.size() == 1u);
    assert(opened.actions[0] == first.rkey);
    const JournalIntent &continued = journal.intents[1];
    assert(continued.id == kThreadUri);
    assert(continued.actions.size() == 2u);
    assert(continued.actions[1] == reply.rkey);
    std::printf("ok record_pending_intent\n");
}

void test_sweep_is_idempotent() {
    const auto root = scratch_dir("sweep");
    const auto path = root / "action-journal.jsonl";

    IntentConfig config;
    config.enabled = true;
    config.max_active = 8u;
    config.max_continuations = 3u;

    /* Two open intents whose window is closed, one still inside its window,
     * and one at the continuation budget. */
    OutboundAction a = sample_action();
    a.rkey = "act-a";
    atperson::record_pending_intent(path, a, executed_result(kThreadUri), config, kOpenAt,
                                    "2026-09-17T10:00:00Z");

    OutboundAction b = sample_action();
    b.rkey = "act-b";
    atperson::record_pending_intent(path, b,
                                    executed_result("at://did:plc:example/app.bsky.feed.post/threadb"),
                                    config, kOpenAt, "2026-09-17T10:00:00Z");

    /* Advance the clock past both windows. */
    const auto later = kOpenAt + 3ll * 24ll * 3600ll; // 2026-09-20
    const IntentSweepReport sweep =
        atperson::sweep_intents(path, later, "2026-09-20T10:00:00Z");
    assert(sweep.evaluated == 2u);
    assert(sweep.open_kept == 0u);
    assert(sweep.expired_written == 2u);
    assert(sweep.closed_written == 0u);
    assert(sweep.already_terminal == 0u);

    const JournalContents after = atperson::load_journal(path);
    assert(atperson::active_intent_count(after, later) == 0u);
    assert(after.intents.size() == 4u);
    for (std::size_t i = 0u; i < 2u; ++i) {
        const JournalIntent &terminal = after.intents[2u + i];
        assert(terminal.state == IntentState::Expired);
        assert(terminal.at == "2026-09-20T10:00:00Z");
    }

    /* Replay the sweep: nothing new is written. */
    const IntentSweepReport replay =
        atperson::sweep_intents(path, later + 1, "2026-09-20T10:00:01Z");
    assert(replay.evaluated == 2u);
    assert(replay.already_terminal == 2u);
    assert(replay.expired_written == 0u);
    const JournalContents stable = atperson::load_journal(path);
    assert(stable.intents.size() == after.intents.size());

    /* A fresh open intent inside its window is left alone. */
    OutboundAction c = sample_action();
    c.rkey = "act-c";
    atperson::record_pending_intent(path, c,
                                    executed_result("at://did:plc:example/app.bsky.feed.post/threadc"),
                                    config, later, "2026-09-20T10:00:00Z");
    const IntentSweepReport partial =
        atperson::sweep_intents(path, later, "2026-09-20T10:00:00Z");
    assert(partial.open_kept == 1u);
    const JournalContents final = atperson::load_journal(path);
    assert(atperson::active_intent_count(final, later) == 1u);

    /* Reconstruct (acceptance): pending intents restore from journal replay. */
    const JournalContents fresh = atperson::load_journal(path);
    assert(fresh.intents.size() == 5u);
    std::printf("ok sweep idempotent\n");
}

void test_sweep_closes_budgeted_intent() {
    const auto root = scratch_dir("sweep-close");
    const auto path = root / "action-journal.jsonl";

    IntentConfig config;
    config.enabled = true;
    config.max_active = 8u;
    config.max_continuations = 2u;

    OutboundAction a = sample_action();
    a.rkey = "act-a";
    atperson::record_pending_intent(path, a, executed_result(kThreadUri), config, kOpenAt,
                                    "2026-09-17T10:00:00Z");
    OutboundAction r1 = sample_action();
    r1.rkey = "act-r1";
    r1.kind = OutboundActionKind::Reply;
    r1.reply_root = kThreadUri;
    r1.reply_parent = kReplyUri;
    atperson::record_pending_intent(path, r1, executed_result(kReplyUri), config, kNextDay,
                                    "2026-09-18T10:00:00Z");
    OutboundAction r2 = sample_action();
    r2.rkey = "act-r2";
    r2.kind = OutboundActionKind::Reply;
    r2.reply_root = kThreadUri;
    r2.reply_parent = kReplyUri;
    atperson::record_pending_intent(path, r2, executed_result(kReplyUri), config, kNextDay,
                                    "2026-09-18T12:00:00Z");

    /* Budget 2 (continuations used = actions.size() - 1 = 2) is reached, so
     * the sweep closes it even inside its window. */
    const IntentSweepReport sweep =
        atperson::sweep_intents(path, kNextDay, "2026-09-18T12:00:00Z");
    assert(sweep.expired_written == 0u);
    assert(sweep.closed_written == 1u);
    const JournalContents journal = atperson::load_journal(path);
    assert(journal.intents.back().state == IntentState::Closed);
    std::printf("ok sweep closes budgeted intent\n");
}

void test_expired_intent_resolves_unmet() {
    const auto root = scratch_dir("resolve-intent");
    const auto path = root / "action-journal.jsonl";

    /* The executed action (an expectation) is inside its 7-day window. */
    atperson::JournalAction action;
    action.id = "act-a";
    action.kind = "post";
    action.text = "the moon is a loyal companion";
    action.digest = "0123456789abcdef";
    action.outcome = atperson::JournalActionOutcome::Executed;
    action.reason = "allow";
    action.uri = kThreadUri;
    action.at = "2026-09-17T10:00:00Z";
    atperson::JournalExpectation expectation;
    expectation.kind = "approach";
    expectation.reply_likelihood = 0.7f;
    expectation.tokens = {"moon"};
    action.expectation = expectation;
    atperson::append_journal_action(path, action);

    /* An intent on that thread whose window has closed with no contact. */
    JournalIntent intent;
    intent.id = kThreadUri;
    intent.actions = {"act-a"};
    intent.responder = "anyone";
    intent.expires_at_epoch = 1789639200; /* already closed at kNextDay */
    intent.max_continuations = 3u;
    intent.state = IntentState::Open;
    intent.at_epoch = 1789639200u;
    intent.at = "2026-09-17T10:00:00Z";
    atperson::append_journal_intent(path, intent);

    const JournalContents journal = atperson::load_journal(path);
    /* 2026-09-18: inside the 7-day window, but the intent expired. */
    const atperson::JournalExpectationState state =
        derive_expectation_state(journal.actions[0], journal.events, kNextDay, journal.intents);
    assert(state == JournalExpectationState::Unmet);
    std::printf("ok expired intent resolves unmet\n");
}

void test_met_contact_beats_intent_expiry() {
    const auto root = scratch_dir("resolve-met");
    const auto path = root / "action-journal.jsonl";

    atperson::JournalAction action;
    action.id = "act-a";
    action.kind = "post";
    action.text = "the moon is a loyal companion";
    action.digest = "0123456789abcdef";
    action.outcome = atperson::JournalActionOutcome::Executed;
    action.reason = "allow";
    action.uri = kThreadUri;
    action.at = "2026-09-17T10:00:00Z";
    atperson::JournalExpectation expectation;
    expectation.kind = "approach";
    expectation.reply_likelihood = 0.7f;
    expectation.tokens = {"moon"};
    action.expectation = expectation;
    atperson::append_journal_action(path, action);

    /* An on-time reply: contact inside the window, even though the intent
     * (opened for the same action) would otherwise be expired. */
    JournalEvent event;
    event.action_id = "act-a";
    event.event_uri = kReplyUri;
    event.author_did = "did:plc:other";
    event.via = "parent";
    event.at = "2026-09-17T12:00:00Z";
    atperson::append_journal_event(path, event);

    JournalIntent intent;
    intent.id = kThreadUri;
    intent.actions = {"act-a"};
    intent.responder = "anyone";
    intent.expires_at_epoch = 1789639200; /* expired immediately */
    intent.max_continuations = 3u;
    intent.state = IntentState::Open;
    intent.at_epoch = 1789639200u;
    intent.at = "2026-09-17T10:00:00Z";
    atperson::append_journal_intent(path, intent);
    const JournalContents journal = atperson::load_journal(path);
    const atperson::JournalExpectationState state =
        derive_expectation_state(journal.actions[0], journal.events, kNextDay, journal.intents);
    assert(state == JournalExpectationState::Met);
    std::printf("ok met contact beats intent expiry\n");
}

} // namespace

int main() {
    test_config_defaults_and_clamps();
    test_config_env_overrides();
    test_config_bounds_are_clamped();
    test_derive_intent_state();
    test_latest_and_active_intents();
    test_continuation_trigger();
    test_record_pending_intent();
    test_sweep_is_idempotent();
    test_sweep_closes_budgeted_intent();
    test_expired_intent_resolves_unmet();
    test_met_contact_beats_intent_expiry();
    std::printf("atperson-intent: all tests passed\n");
    return 0;
}