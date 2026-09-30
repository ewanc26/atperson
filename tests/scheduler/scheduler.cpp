/* Autonomous scheduler (#140): decision -> proposal -> approved execution,
 * composed through the existing gate chain, plus the pending-social-intent
 * (#150) conversation lifecycle — an executed post opens an intent, the
 * invited reply is composed as a continuation reply, and a closed window
 * expires the intent and resolves its action unmet. Offline: the network is a
 * fake OutboundWriter, the clock is injected, the ledger is real. */
#include "autonomy/arming.hpp"
#include "scheduler/cycle.hpp"

#include "action/inspection.hpp"
#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/actions.hpp"
#include "outbound/audit.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using atperson::ControlState;
using atperson::IntentState;
using atperson::JournalContents;
using atperson::JournalExpectationState;
using atperson::JournalIntent;
using atperson::LanguageGraph;
using atperson::Ledger;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::OutboundPolicy;
using atperson::OutboundWriteResult;
using atperson::OutboundWriter;
using atperson::SchedulerConfig;
using atperson::SchedulerCycle;
using atperson::SchedulerCycleReport;

constexpr std::int64_t NOW = 1'700'000'000;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-scheduler-" + std::string(tag));
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
    std::vector<std::string> rkeys;

    std::string resolve_record_cid(const std::string &) override { return "bafycid"; }

    OutboundWriteResult put_record(const std::string &, const std::string &rkey,
                                   const std::string &) override {
        ++put_calls;
        rkeys.push_back(rkey);
        OutboundWriteResult written;
        written.uri = "at://did:plc:self/app.bsky.feed.post/" + rkey;
        written.cid = "bafyrecord";
        return written;
    }
};

struct FlakyWriter final : OutboundWriter {
    int put_calls = 0;
    bool fail = true;

    std::string resolve_record_cid(const std::string &) override { return "bafycid"; }

    OutboundWriteResult put_record(const std::string &, const std::string &rkey,
                                   const std::string &) override {
        ++put_calls;
        if (fail) {
            throw std::runtime_error("network down");
        }
        OutboundWriteResult written;
        written.uri = "at://did:plc:self/app.bsky.feed.post/" + rkey;
        written.cid = "bafyrecord";
        return written;
    }
};

/* Gate files: policy allows posts, control has writes enabled with
 * approval required (the fail-closed default posture). */
struct GateFiles {
    std::filesystem::path root;
    std::filesystem::path policy;
    std::filesystem::path budget;
    std::filesystem::path control;
    std::filesystem::path audit;
    std::filesystem::path journal;
    std::filesystem::path envelopes;

    GateFiles(const char *tag)
        : root(scratch_dir(tag)), policy(root / "policy.json"), budget(root / "budget.json"),
          control(root / "control.json"), audit(root / "audit.log"),
          journal(root / "journal.jsonl"), envelopes(root / "envelopes") {
        atperson::OutboundPolicy policy_state;
        atperson::ActionBudget post_budget;
        post_budget.enabled = true;
        post_budget.max_in_window = 10;
        post_budget.window_seconds = 3600;
        post_budget.min_interval_seconds = 0;
        post_budget.duplicate_cooldown_seconds = 0;
        atperson::budget_for(policy_state, OutboundActionKind::Post) = post_budget;
        atperson::ActionBudget reply_budget;
        reply_budget.enabled = true;
        reply_budget.max_in_window = 10;
        reply_budget.window_seconds = 3600;
        reply_budget.min_interval_seconds = 0;
        reply_budget.duplicate_cooldown_seconds = 0;
        atperson::budget_for(policy_state, OutboundActionKind::Reply) = reply_budget;
        write_file(policy, atperson::serialise_outbound_policy(policy_state));

        ControlState control_state;
        control_state.paused = false;
        control_state.writes_enabled = true;
        control_state.dry_run = false;
        control_state.approval_required = true;
        atperson::save_control_state(control_state, control);
    }
};

/* A graph where "alpha beta" is strongly learned, and a ledger whose most
 * recent committed payload is "alpha": the decision layer accepts the
 * continuation "beta". */
LanguageGraph learned_graph() {
    LanguageGraph graph;
    for (std::size_t i = 0u; i < 8u; ++i) {
        graph.observe("alpha beta", "at://scheduler/observe/" + std::to_string(i));
    }
    return graph;
}

Ledger populated_ledger(const std::filesystem::path &root) {
    Ledger ledger(root / "ledger.bin");
    const std::uint64_t digest = Ledger::digest("alpha");
    std::uint64_t id = 0u;
    ledger.append("at://scheduler/context/1", "did:plc:other", NOW, digest, 1u,
                  ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id);
    return ledger;
}

SchedulerCycle make_cycle(const GateFiles &gates, const std::filesystem::path &data_dir,
                          OutboundWriter &writer, std::int64_t now = NOW) {
    return SchedulerCycle{
        data_dir,
        data_dir / "proposals",
        atperson::OutboundAttemptPaths{gates.policy, gates.budget, gates.control, gates.audit,
                                        gates.journal, gates.envelopes},
        [&writer]() -> OutboundWriter & { return writer; },
        now,
        []() -> std::int64_t { return 0; }};
}

SchedulerConfig enabled_config() {
    SchedulerConfig config;
    config.enabled = true;
    return config;
}

void test_disabled_scheduler_is_inert() {
    const GateFiles gates("disabled");
    FakeWriter writer;
    LanguageGraph graph;
    Ledger ledger = populated_ledger(gates.root);
    SchedulerConfig config;
    config.enabled = false;

    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        config, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.contexts_examined == 0u);
    assert(report.proposals_written == 0u);
    assert(writer.put_calls == 0);
}

void test_decision_writes_proposal_but_never_executes_unapproved() {
    const GateFiles gates("proposal");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);

    /* The decision accepted the "beta" continuation and froze it as an
     * inspectable proposal; approval is required, so nothing executed. */
    assert(report.contexts_examined == 1u);
    assert(report.decisions == 1u);
    assert(report.abstentions == 0u);
    assert(report.proposals_written == 1u);
    assert(report.executions_attempted == 0u);
    assert(report.executed == 0u);
    assert(writer.put_calls == 0);

    /* The proposal is a valid action document the operator can inspect and
     * `atperson publish` can consume unchanged. */
    std::filesystem::path proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        proposal = entry.path();
    }
    const OutboundAction action = atperson::load_outbound_action(proposal);
    assert(action.kind == OutboundActionKind::Post);
    assert(action.text == "beta");
    assert(action.digest.size() == 16u);
}

void test_approved_proposal_executes_and_is_consumed() {
    const GateFiles gates("approved");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    /* Cycle 1: freeze the proposal. */
    atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer), graph,
                                 ledger);
    std::filesystem::path proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        proposal = entry.path();
    }
    const OutboundAction action = atperson::load_outbound_action(proposal);

    /* The operator approves the exact digest. */
    ControlState control = atperson::load_control_state(gates.control);
    control.approved_digests.push_back(action.digest);
    atperson::save_control_state(control, gates.control);

    /* Cycle 2: the approved proposal executes through the full gate chain
     * and is consumed. */
    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.executions_attempted == 1u);
    assert(report.executed == 1u);
    assert(writer.put_calls == 1);
    assert(writer.rkeys.front() == action.rkey);
    assert(!std::filesystem::exists(proposal));

    /* Durable bookkeeping: audit and journal recorded the execution. */
    const JournalContents journal = atperson::load_journal(gates.journal);
    assert(journal.actions.size() == 1u);
    assert(journal.actions.front().outcome == atperson::JournalActionOutcome::Executed);
    assert(journal.actions.front().text == "beta");
}

void test_pause_between_cycles_refuses_execution() {
    const GateFiles gates("paused");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer), graph,
                                  ledger);
    std::filesystem::path proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        proposal = entry.path();
    }
    const OutboundAction action = atperson::load_outbound_action(proposal);

    ControlState control = atperson::load_control_state(gates.control);
    control.approved_digests.push_back(action.digest);
    control.paused = true;
    atperson::save_control_state(control, gates.control);

    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    /* The pause gate refuses before the write; the refusal is recorded,
     * the proposal stays for the operator. */
    assert(report.executions_attempted == 1u);
    assert(report.executed == 0u);
    assert(report.refused == 1u);
    assert(writer.put_calls == 0);
    assert(std::filesystem::exists(proposal));
}

void test_existing_proposal_is_never_rewritten() {
    const GateFiles gates("existing");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    const SchedulerCycleReport first = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(first.proposals_written == 1u);

    /* Second cycle over the same context: the digest matches, the exact
     * bytes are already frozen, so nothing is rewritten. */
    const SchedulerCycleReport second = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(second.decisions == 1u);
    assert(second.proposals_written == 0u);
    assert(second.proposals_existing == 1u);
}

void test_abstention_writes_no_proposal() {
    const GateFiles gates("abstain");
    FakeWriter writer;
    LanguageGraph graph; /* empty: no learned continuations */
    Ledger ledger = populated_ledger(gates.root);

    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.contexts_examined == 1u);
    assert(report.abstentions == 1u);
    assert(report.decisions == 0u);
    assert(report.proposals_written == 0u);
}

/* #152: with graduated likes enabled, a below-floor abstention on a post
 * record composes an explicit like proposal with its own digest; the
 * abstention is still counted, and nothing is executed without approval. */
void test_graduated_like_from_below_floor_abstention() {
    const GateFiles gates("graduated");
    FakeWriter writer;
    LanguageGraph graph; /* empty: decisions abstain */
    Ledger ledger(gates.root / "ledger.bin");
    std::uint64_t id = 0u;
    ledger.append("at://did:plc:author/app.bsky.feed.post/3kabc", "did:plc:author", NOW,
                  Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id);

    SchedulerConfig config = enabled_config();
    config.graduated_likes = true;
    config.engagement.mode = atperson::EngagementMode::Open; /* consent is tested separately */
    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        config, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.contexts_examined == 1u);
    assert(report.abstentions == 1u);
    assert(report.decisions == 0u);
    assert(report.proposals_written == 1u);
    assert(report.graduated_likes_written == 1u);
    assert(writer.put_calls == 0);

    /* The proposal document is a like bound to the subject record. */
    bool found = false;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream file(entry.path());
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        assert(text.find("\"kind\":\"like\"") != std::string::npos);
        assert(text.find("at://did:plc:author/app.bsky.feed.post/3kabc") != std::string::npos);
        found = true;
    }
    assert(found);
}

/* #152: graduated likes never fire without the operator flag, and a
 * non-post source (no likeable subject) abstains without a proposal. */
void test_graduated_like_requires_flag_and_likeable_source() {
    {
        const GateFiles gates("graduated-off");
        FakeWriter writer;
        LanguageGraph graph;
        Ledger ledger(gates.root / "ledger.bin");
        std::uint64_t id = 0u;
        ledger.append("at://did:plc:author/app.bsky.feed.post/3kabc", "did:plc:author", NOW,
                      Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id);
        const SchedulerCycleReport report = atperson::run_scheduler_cycle(
            enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
        assert(report.abstentions == 1u);
        assert(report.proposals_written == 0u);
        assert(report.graduated_likes_written == 0u);
    }
    {
        const GateFiles gates("graduated-unlikeable");
        FakeWriter writer;
        LanguageGraph graph;
        Ledger ledger(gates.root / "ledger.bin");
        std::uint64_t id = 0u;
        ledger.append("at://did:plc:author/custom.record/3kabc", "did:plc:author", NOW,
                      Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id);
        SchedulerConfig config = enabled_config();
        config.graduated_likes = true;
        const SchedulerCycleReport report = atperson::run_scheduler_cycle(
            config, make_cycle(gates, gates.root, writer), graph, ledger);
        assert(report.abstentions == 1u);
        assert(report.proposals_written == 0u);
        assert(report.graduated_likes_written == 0u);
    }
}

/* The first decision slot is the bounded initiation surface: with max one
 * proposal per cycle, drive ordering (#148) decides *which* context gets it.
 * The reply referencing an executed action must beat a newer, unrelated post. */
void test_drives_reorder_first_decision_context() {
    const GateFiles off_gates("drives-off");
    const GateFiles on_gates("drives-on");
    FakeWriter writer;
    LanguageGraph graph;
    for (std::size_t i = 0u; i < 8u; ++i) {
        graph.observe("alpha beta", "at://drives/observe/a/" + std::to_string(i));
        graph.observe("foo bar", "at://drives/observe/b/" + std::to_string(i));
    }

    const auto run_with = [&](const GateFiles &gates, bool drives_enabled) -> SchedulerCycleReport {
        Ledger ledger(gates.root / "ledger.bin");
        std::uint64_t id = 0u;
        /* Older: the reply that referenced an executed action (reciprocity). */
        assert(ledger.append("at://scheduler/reply/2", "did:plc:fan", NOW,
                             Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha",
                             &id) == atperson::LedgerResult::New);
        /* Newest: an unrelated post with no journal link. */
        assert(ledger.append("at://scheduler/post/9", "did:plc:fan", NOW, Ledger::digest("foo"),
                             1u, ATP_LEDGER_OUTCOME_LEARNED, "foo", &id) ==
               atperson::LedgerResult::New);
        atperson::JournalEvent event;
        event.action_id = "reply-rkey";
        event.event_uri = "at://scheduler/reply/2";
        event.author_did = "did:plc:fan";
        event.via = "reply";
        event.at = "2023-11-15T00:00:00Z";
        atperson::append_journal_event(gates.journal, event);

        SchedulerConfig config = enabled_config();
        config.drives_enabled = drives_enabled;
        const SchedulerCycleReport report =
            atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer), graph,
                                          ledger);
        assert(report.contexts_examined == 2u);
        assert(report.decisions == 1u);
        assert(report.proposals_written == 1u);
        return report;
    };

    /* Without drives: the newer unrelated post ("foo") wins the slot. */
    const SchedulerCycleReport off = run_with(off_gates, false);
    assert(!off.ordered_by_drives);
    std::string off_text;
    for (const auto &entry : std::filesystem::directory_iterator(off_gates.root / "proposals")) {
        off_text = atperson::load_outbound_action(entry.path()).text;
    }
    assert(off_text == "bar");

    /* With drives: the reciprocated reply context ("alpha") wins the slot. */
    const SchedulerCycleReport on = run_with(on_gates, true);
    assert(on.ordered_by_drives);
    std::string on_text;
    for (const auto &entry : std::filesystem::directory_iterator(on_gates.root / "proposals")) {
        on_text = atperson::load_outbound_action(entry.path()).text;
    }
    assert(on_text == "beta");
}

/* #150 end-to-end through the cycle: two executed posts open two pending
 * intents; the invited reply on one conversation is composed as a
 * continuation reply (reply_root = the thread, reply_parent = the triggered
 * observation) and, once approved, executes into the same conversation; and a
 * closed window later expires both intents while the resolution pass records
 * the unanswered conversation's action unmet. Everything above drives through
 * the real gate chain with a fake writer; nothing is simulated. The two
 * conversations and the continuation use distinct learned plans so each
 * frozen proposal and its action id are distinct. */
void test_intent_conversation_lifecycle() {
    const GateFiles gates("intents");
    FakeWriter writer;

    LanguageGraph graph = learned_graph();
    for (std::size_t i = 0u; i < 8u; ++i) {
        graph.observe("foo bar", "at://scheduler/foo/" + std::to_string(i));
        graph.observe("gamma delta", "at://scheduler/gamma/" + std::to_string(i));
    }

    Ledger ledger(gates.root / "ledger.bin");
    std::uint64_t id = 0u;
    assert(ledger.append("at://scheduler/context/1", "did:plc:other", NOW,
                         Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha",
                         &id) == atperson::LedgerResult::New);
    assert(ledger.append("at://scheduler/context/2", "did:plc:other", NOW + 1,
                         Ledger::digest("foo"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "foo",
                         &id) == atperson::LedgerResult::New);

    SchedulerConfig base = enabled_config();
    base.intents.enabled = true;
    base.intents.max_active = 3u;
    base.intents.max_continuations = 3u;
    base.intents.window_seconds = 30 * 24 * 3600;
    base.max_contexts = 2u;
    base.max_proposals = 4u;
    base.max_executions = 4u;

    /* Phase A: cycle 1 freezes both decisions; cycle 2 executes the approved
     * posts and opens one intent per executed record URI. */
    const SchedulerCycleReport seeded = atperson::run_scheduler_cycle(
        base, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(seeded.proposals_written == 2u);
    ControlState control = atperson::load_control_state(gates.control);
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        control.approved_digests.push_back(atperson::load_outbound_action(entry.path()).digest);
    }
    atperson::save_control_state(control, gates.control);

    const SchedulerCycleReport second = atperson::run_scheduler_cycle(
        base, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(second.executed == 2u);
    assert(second.intents_opened == 2u);

    const JournalContents open = atperson::load_journal(gates.journal);
    assert(open.actions.size() == 2u);
    assert(open.intents.size() == 2u);
    std::string beta_rkey;
    std::string foo_rkey;
    for (const auto &action : open.actions) {
        if (action.text == "beta") {
            beta_rkey = action.id;
        } else if (action.text == "bar") {
            foo_rkey = action.id;
        }
    }
    assert(!beta_rkey.empty() && !foo_rkey.empty());
    const JournalIntent *beta_intent = nullptr;
    const JournalIntent *foo_intent = nullptr;
    for (const auto &intent : open.intents) {
        assert(intent.state == IntentState::Open);
        if (intent.id.find("/" + beta_rkey) != std::string::npos) {
            beta_intent = &intent;
        } else if (intent.id.find("/" + foo_rkey) != std::string::npos) {
            foo_intent = &intent;
        }
    }
    assert(beta_intent != nullptr && foo_intent != nullptr);

    /* Phase B: a reply arrives on the first conversation. The single newest
     * observation is exactly the event the open intent is waiting on, so the
     * proposal is composed as a continuation reply with a distinct plan. */
    assert(ledger.append("at://scheduler/reply/2", "did:plc:fan", NOW + 2,
                         Ledger::digest("gamma"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "gamma",
                         &id) == atperson::LedgerResult::New);
    atperson::JournalEvent arrived;
    arrived.action_id = beta_rkey;
    arrived.event_uri = "at://scheduler/reply/2";
    arrived.author_did = "did:plc:fan";
    arrived.via = "reply";
    arrived.at = "2023-11-14T22:23:20Z"; /* NOW + 600s: inside the window */
    atperson::append_journal_event(gates.journal, arrived);

    SchedulerConfig narrow = base;
    narrow.max_contexts = 1u;
    narrow.max_proposals = 1u;

    const SchedulerCycleReport third = atperson::run_scheduler_cycle(
        narrow, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(third.ordered_by_intents);
    assert(third.contexts_examined == 1u);
    assert(third.intents_evaluated == 2u);
    assert(third.intents_expired == 0u);
    assert(third.proposals_written == 1u);

    /* The on-time reply resolves the first prediction met; the second is
     * still pending. */
    const JournalContents reasoned = atperson::load_journal(gates.journal);
    assert(reasoned.resolutions.size() == 1u);
    assert(reasoned.resolutions[0].action_id == beta_rkey);
    assert(reasoned.resolutions[0].state == JournalExpectationState::Met);

    std::filesystem::path reply_proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        reply_proposal = entry.path();
    }
    const OutboundAction continuation = atperson::load_outbound_action(reply_proposal);
    assert(continuation.kind == OutboundActionKind::Reply);
    assert(continuation.reply_root == beta_intent->id);
    assert(continuation.reply_parent == "at://scheduler/reply/2");
    assert(continuation.text == "delta");
    assert(continuation.rkey != beta_rkey && continuation.rkey != foo_rkey);

    /* Approve the reply and let the next cycle execute it into the same
     * conversation. */
    control = atperson::load_control_state(gates.control);
    control.approved_digests.push_back(continuation.digest);
    atperson::save_control_state(control, gates.control);
    const SchedulerCycleReport fourth = atperson::run_scheduler_cycle(
        narrow, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(fourth.executed == 1u);
    assert(fourth.intents_continued == 1u);
    assert(!std::filesystem::exists(reply_proposal));

    const JournalContents grown = atperson::load_journal(gates.journal);
    const JournalIntent *continued_beta = nullptr;
    for (const auto &intent : grown.intents) {
        if (intent.id == beta_intent->id) {
            continued_beta = &intent;
        }
    }
    assert(continued_beta != nullptr);
    assert(continued_beta->state == IntentState::Open);
    assert(continued_beta->actions.size() == 2u);
    assert(continued_beta->actions[1] == continuation.rkey);

    /* Phase C: the window closes with no further reply. Both intents expire;
     * the sweep journals the terminal states and the resolution pass records
     * the unanswered (second) conversation's action unmet. */
    SchedulerConfig inert = base;
    inert.max_contexts = 2u;
    inert.max_proposals = 0u;
    inert.max_executions = 0u;
    const std::int64_t later = NOW + 31 * 24 * 3600;
    const SchedulerCycleReport fifth = atperson::run_scheduler_cycle(
        inert, make_cycle(gates, gates.root, writer, later), graph, ledger);
    assert(fifth.intents_evaluated == 2u);
    assert(fifth.intents_expired == 2u);
    assert(fifth.intents_closed == 0u);
    (void)foo_intent;

    const JournalContents terminal = atperson::load_journal(gates.journal);
    std::size_t beta_terminal = 0u;
    std::size_t foo_terminal = 0u;
    for (const auto &intent : terminal.intents) {
        if (intent.id == beta_intent->id) {
            beta_terminal += intent.state == IntentState::Expired ? 1u : 0u;
        }
        if (intent.id == foo_intent->id) {
            foo_terminal += intent.state == IntentState::Expired ? 1u : 0u;
        }
    }
    assert(beta_terminal >= 1u && foo_terminal >= 1u);
    bool unanswered_unmet = false;
    for (const auto &entry : terminal.resolutions) {
        if (entry.action_id == foo_rkey && entry.state == JournalExpectationState::Unmet) {
            unanswered_unmet = true;
        }
    }
    assert(unanswered_unmet);
}

} // namespace

/* Unattended operation end to end: after the one-time arming, decisions turn
 * into published records with no per-action approval anywhere, every step
 * still passes the gate chain, and the operator's kill switches still stop it. */
atperson::ArmPaths arm_gates(const GateFiles &gates) {
    atperson::ArmRequest request;
    request.kinds = {{OutboundActionKind::Post, 5, 3600}};
    atperson::ArmPaths paths{gates.policy, gates.control, gates.envelopes};
    /* Start from the fail-closed posture a fresh install has: writes off, dry-run on. */
    atperson::ControlState fresh;
    atperson::save_control_state(fresh, gates.control);
    atperson::apply_arm(atperson::plan_arm(request, atperson::load_outbound_policy(gates.policy),
                                           fresh, "2026-09-17T10:00:00Z"),
                        paths);
    return paths;
}

void test_armed_entity_publishes_with_no_human_step() {
    const GateFiles gates("armed");
    arm_gates(gates);
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    /* No control approve anywhere. A couple of cycles is all it takes to
     * decide, freeze and publish. */
    for (int cycle = 0; cycle < 2; ++cycle) {
        atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer), graph,
                                     ledger);
    }
    assert(writer.put_calls == 1);
    assert(atperson::load_control_state(gates.control).approved_digests.empty());

    /* Exactly one record was published. A later cycle may re-decide the same
     * context; the policy's duplicate suppression refuses it, and that refusal is
     * itself journaled as a deferred attempt, never as a second publish. */
    const JournalContents journal = atperson::load_journal(gates.journal);
    std::size_t executed = 0u;
    for (const auto &action : journal.actions) {
        if (action.outcome == atperson::JournalActionOutcome::Executed) {
            ++executed;
            assert(action.text == "beta");
        } else {
            /* A repeat is refused by duplicate suppression, not published. */
            assert(action.outcome == atperson::JournalActionOutcome::Deferred);
            assert(action.reason == "duplicate_suppressed");
        }
    }
    assert(executed == 1u);

    /* The authorisation is on the record: the audit entry names the envelope. */
    std::ifstream audit(gates.audit);
    const std::string log((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
    assert(log.find("autonomy") != std::string::npos);

    /* Consumed exactly once: further cycles publish nothing more. */
    for (int cycle = 0; cycle < 2; ++cycle) {
        atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer), graph,
                                     ledger);
    }
    assert(writer.put_calls == 1);
}

void test_armed_entity_still_obeys_pause_and_disarm() {
    /* Pause holds the proposal; resuming lets the same frozen proposal go out
     * with no approval. */
    {
        const GateFiles gates("armed-pause");
        const atperson::ArmPaths paths = arm_gates(gates);
        FakeWriter writer;
        LanguageGraph graph = learned_graph();
        Ledger ledger = populated_ledger(gates.root);

        ControlState control = atperson::load_control_state(gates.control);
        control.paused = true;
        atperson::save_control_state(control, gates.control);
        for (int cycle = 0; cycle < 2; ++cycle) {
            atperson::run_scheduler_cycle(enabled_config(),
                                         make_cycle(gates, gates.root, writer), graph, ledger);
        }
        assert(writer.put_calls == 0);

        control.paused = false;
        atperson::save_control_state(control, gates.control);
        atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer),
                                     graph, ledger);
        assert(writer.put_calls == 1);
        (void)paths;
    }
    /* Disarming revokes the authorisation: nothing goes out, not even the
     * proposals the entity keeps making. */
    {
        const GateFiles gates("armed-disarm");
        const atperson::ArmPaths paths = arm_gates(gates);
        atperson::disarm(paths, "autonomy");
        FakeWriter writer;
        LanguageGraph graph = learned_graph();
        Ledger ledger = populated_ledger(gates.root);
        for (int cycle = 0; cycle < 3; ++cycle) {
            atperson::run_scheduler_cycle(enabled_config(),
                                         make_cycle(gates, gates.root, writer), graph, ledger);
        }
        assert(writer.put_calls == 0);
    }
}

/* Output guard (text_guard.hpp) at the scheduler: a denylisted word never
 * becomes a proposal, lifting the term lets it through on the next cycle
 * (the file is re-read every cycle), and the same words are never published
 * twice. */
void test_output_guard_refuses_denied_text_and_recovers_when_lifted() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("guard-denied");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    write_file(gates.root / "output-denylist.txt", "# never say\nBETA\n");

    SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.decisions == 1u);
    assert(report.proposals_written == 0u);
    assert(report.text_refused == 1u && report.last_text_refusal == "denied_term");
    assert(!std::filesystem::exists(gates.root / "proposals") ||
           std::filesystem::is_empty(gates.root / "proposals"));

    /* The operator lifts the term; no restart needed. */
    std::filesystem::remove(gates.root / "output-denylist.txt");
    report = atperson::run_scheduler_cycle(enabled_config(),
                                           make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.proposals_written == 1u && report.text_refused == 0u);
}

void test_output_guard_never_repeats_published_text() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("guard-repeat");
    arm_gates(gates);
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    for (int cycle = 0; cycle < 2; ++cycle) {
        atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer),
                                     graph, ledger);
    }
    assert(writer.put_calls == 1);

    /* The same context decides the same words again: refused before it is even
     * proposed, so nothing is queued and no refused attempt piles up. */
    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.text_refused == 1u && report.last_text_refusal == "repeated_text");
    assert(report.proposals_written == 0u && report.executions_attempted == 0u);
    assert(writer.put_calls == 1);

    /* Turning the window off (0) is an explicit operator choice. */
    SchedulerConfig open_window = enabled_config();
    open_window.text_guard.repeat_window_seconds = 0;
    const SchedulerCycleReport allowed = atperson::run_scheduler_cycle(
        open_window, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(allowed.text_refused == 0u && allowed.proposals_written == 1u);
}

void test_output_guard_rechecks_frozen_proposals_at_execution() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("guard-exec");
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);

    atperson::run_scheduler_cycle(enabled_config(), make_cycle(gates, gates.root, writer), graph,
                                 ledger);
    std::filesystem::path proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        proposal = entry.path();
    }
    const OutboundAction action = atperson::load_outbound_action(proposal);
    ControlState control = atperson::load_control_state(gates.control);
    control.approved_digests.push_back(action.digest);
    atperson::save_control_state(control, gates.control);

    /* Approved, but the operator has since denylisted the word: it must not go
     * out, and the frozen proposal stays where it is for inspection. */
    write_file(gates.root / "output-denylist.txt", "beta\n");
    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        enabled_config(), make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.text_refused >= 1u && report.last_text_refusal == "denied_term");
    assert(report.executions_attempted == 0u);
    assert(writer.put_calls == 0);
    assert(std::filesystem::exists(proposal));
}

void test_output_guard_repeat_window_comes_from_the_environment() {
    unsetenv("ATPERSON_OUTPUT_REPEAT_WINDOW");
    assert(atperson::scheduler_config_from_environment().text_guard.repeat_window_seconds ==
           7ll * 24 * 3600);
    setenv("ATPERSON_OUTPUT_REPEAT_WINDOW", "3600", 1);
    assert(atperson::scheduler_config_from_environment().text_guard.repeat_window_seconds == 3600);
    setenv("ATPERSON_OUTPUT_REPEAT_WINDOW", "0", 1);
    assert(atperson::scheduler_config_from_environment().text_guard.repeat_window_seconds == 0);
    for (const char *bad : {"-1", "abc", "1.5", "10x"}) {
        setenv("ATPERSON_OUTPUT_REPEAT_WINDOW", bad, 1);
        bool threw = false;
        try {
            (void)atperson::scheduler_config_from_environment();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
    unsetenv("ATPERSON_OUTPUT_REPEAT_WINDOW");
}

/* Circuit breaker (breaker.hpp) at the scheduler: repeated genuine failures stop
 * the hammering, the cool-down elapsing allows a single probe, and a probe that
 * works closes it again with no human involved. */
SchedulerConfig breaker_config(std::uint32_t threshold, std::uint32_t proposal_limit) {
    SchedulerConfig config = enabled_config();
    config.breaker.failure_threshold = threshold;
    config.breaker.base_cooldown_seconds = 900;
    config.breaker.max_cooldown_seconds = 7200;
    config.breaker.proposal_failure_limit = proposal_limit;
    return config;
}

void test_breaker_opens_on_repeated_failures_and_heals_itself() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("breaker");
    arm_gates(gates);
    FlakyWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    const SchedulerConfig config = breaker_config(2, 100);
    auto run = [&](std::int64_t now) {
        return atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer, now),
                                             graph, ledger);
    };

    SchedulerCycleReport report = run(NOW);
    assert(report.failed == 1u && report.breaker_gate == "closed" && report.breaker_trips == 0u);
    report = run(NOW + 1); /* the second consecutive failure opens it */
    assert(report.failed == 1u && report.breaker_trips == 1u);
    assert(writer.put_calls == 2);

    /* Held back for the whole cool-down: nothing reaches the network, and the
     * cycle says why. Deciding and proposing carry on. */
    for (const std::int64_t offset : {2, 100, 899}) {
        report = run(NOW + 1 + offset);
        assert(report.breaker_gate == "open" && report.executions_attempted == 0u);
        assert(report.detail.find("circuit breaker open") != std::string::npos);
    }
    assert(writer.put_calls == 2);

    /* Half-open: the cool-down elapsed. One probe; the dependency is back. */
    writer.fail = false;
    report = run(NOW + 1 + 900);
    assert(report.breaker_gate == "half-open");
    assert(report.executions_attempted == 1u && report.executed == 1u);
    assert(writer.put_calls == 3);
    const atperson::BreakerState healed = atperson::load_breaker_state(gates.root / "scheduler-breaker.json");
    assert(atperson::breaker_gate(healed, NOW + 5000) == atperson::BreakerGate::Closed);
    assert(healed.consecutive_failures == 0u && healed.cooldown_seconds == 0);
}

void test_a_failed_probe_reopens_with_a_longer_cooldown() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("breaker-probe");
    arm_gates(gates);
    FlakyWriter writer; /* stays down */
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    const SchedulerConfig config = breaker_config(1, 100);
    auto run = [&](std::int64_t now) {
        return atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer, now),
                                             graph, ledger);
    };
    run(NOW); /* opens at once: threshold 1 */
    assert(atperson::load_breaker_state(gates.root / "scheduler-breaker.json").cooldown_seconds ==
           900);
    const SchedulerCycleReport probe = run(NOW + 900);
    assert(probe.breaker_gate == "half-open" && probe.breaker_trips == 1u);
    const atperson::BreakerState reopened = atperson::load_breaker_state(gates.root / "scheduler-breaker.json");
    assert(reopened.cooldown_seconds == 1800 && reopened.trips == 2u);
    assert(writer.put_calls == 2); /* exactly one probe, then quiet again */
    assert(run(NOW + 901).executions_attempted == 0u);
}

/* Policy, control and guard refusals are the system working, not faults: they
 * must never trip the breaker. */
void test_refusals_do_not_trip_the_breaker() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("breaker-refusals");
    arm_gates(gates);
    ControlState control = atperson::load_control_state(gates.control);
    control.paused = true;
    atperson::save_control_state(control, gates.control);
    FlakyWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    const SchedulerConfig config = breaker_config(1, 1);
    for (int cycle = 0; cycle < 5; ++cycle) {
        const SchedulerCycleReport report = atperson::run_scheduler_cycle(
            config, make_cycle(gates, gates.root, writer, NOW + cycle), graph, ledger);
        assert(report.failed == 0u && report.breaker_trips == 0u && report.quarantined == 0u);
    }
    assert(writer.put_calls == 0);
    assert(!std::filesystem::exists(gates.root / "scheduler-breaker.json"));
}

void test_a_poison_proposal_is_quarantined_and_never_reproposed() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("quarantine");
    arm_gates(gates);
    FlakyWriter writer;
    LanguageGraph graph = learned_graph();
    Ledger ledger = populated_ledger(gates.root);
    const SchedulerConfig config = breaker_config(100, 2); /* quarantine before any trip */
    auto run = [&](std::int64_t now) {
        return atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer, now),
                                             graph, ledger);
    };
    assert(run(NOW).quarantined == 0u);
    const SchedulerCycleReport report = run(NOW + 1);
    assert(report.quarantined == 1u && report.breaker_trips == 0u);
    assert(writer.put_calls == 2);

    /* Set aside with the reason, out of the live queue. */
    std::size_t live = 0;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        live += entry.is_regular_file() ? 1u : 0u;
    }
    assert(live == 0u);
    std::size_t quarantined = 0;
    bool has_reason = false;
    for (const auto &entry :
         std::filesystem::directory_iterator(gates.root / "proposals" / "quarantine")) {
        quarantined += entry.path().extension() == ".json" ? 1u : 0u;
        has_reason = has_reason || entry.path().extension() == ".reason";
    }
    assert(quarantined == 1u && has_reason);

    /* The same context decides the same words again; it is not re-proposed, so
     * the failure does not repeat forever. */
    for (int cycle = 2; cycle < 6; ++cycle) {
        const SchedulerCycleReport later = run(NOW + cycle);
        assert(later.proposals_written == 0u && later.executions_attempted == 0u);
    }
    assert(writer.put_calls == 2);
}

void test_the_pending_queue_is_capped() {
    unsetenv("ATPERSON_OUTPUT_DENYLIST");
    const GateFiles gates("queue-cap"); /* approval required: proposals wait */
    FakeWriter writer;
    LanguageGraph graph = learned_graph();
    for (std::size_t i = 0u; i < 8u; ++i) {
        graph.observe("gamma delta", "at://scheduler/observe-2/" + std::to_string(i));
    }
    Ledger ledger = populated_ledger(gates.root);
    const std::uint64_t digest = Ledger::digest("gamma");
    std::uint64_t id = 0u;
    ledger.append("at://scheduler/context/2", "did:plc:other", NOW, digest, 1u,
                  ATP_LEDGER_OUTCOME_LEARNED, "gamma", &id);

    SchedulerConfig config = enabled_config();
    config.max_pending_proposals = 1;
    SchedulerCycleReport report = atperson::run_scheduler_cycle(
        config, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.proposals_written == 1u && report.proposals_capped == 0u);
    report = atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer), graph,
                                           ledger);
    assert(report.proposals_written == 0u && report.proposals_capped == 1u);

    config.max_pending_proposals = 5; /* room again */
    report = atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer), graph,
                                           ledger);
    assert(report.proposals_written == 1u);
}

void test_breaker_settings_come_from_the_environment() {
    for (const char *name : {"ATPERSON_BREAKER_THRESHOLD", "ATPERSON_BREAKER_COOLDOWN",
                             "ATPERSON_BREAKER_MAX_COOLDOWN", "ATPERSON_PROPOSAL_FAILURE_LIMIT",
                             "ATPERSON_SCHEDULER_MAX_PENDING"}) {
        unsetenv(name);
    }
    const SchedulerConfig defaults = atperson::scheduler_config_from_environment();
    assert(defaults.breaker.failure_threshold == 3u && defaults.breaker.base_cooldown_seconds == 900);
    assert(defaults.max_pending_proposals == 50u);

    setenv("ATPERSON_BREAKER_THRESHOLD", "5", 1);
    setenv("ATPERSON_BREAKER_COOLDOWN", "60", 1);
    setenv("ATPERSON_BREAKER_MAX_COOLDOWN", "600", 1);
    setenv("ATPERSON_PROPOSAL_FAILURE_LIMIT", "4", 1);
    setenv("ATPERSON_SCHEDULER_MAX_PENDING", "7", 1);
    const SchedulerConfig tuned = atperson::scheduler_config_from_environment();
    assert(tuned.breaker.failure_threshold == 5u && tuned.breaker.base_cooldown_seconds == 60);
    assert(tuned.breaker.max_cooldown_seconds == 600 && tuned.breaker.proposal_failure_limit == 4u);
    assert(tuned.max_pending_proposals == 7u);

    for (const auto &[name, bad] :
         {std::pair<const char *, const char *>{"ATPERSON_BREAKER_THRESHOLD", "0"},
          {"ATPERSON_BREAKER_COOLDOWN", "-5"},
          {"ATPERSON_BREAKER_MAX_COOLDOWN", "30"}, /* below the cool-down (60) */
          {"ATPERSON_PROPOSAL_FAILURE_LIMIT", "abc"},
          {"ATPERSON_SCHEDULER_MAX_PENDING", "0"}}) {
        setenv(name, bad, 1);
        bool threw = false;
        try {
            (void)atperson::scheduler_config_from_environment();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
        unsetenv(name);
        /* restore the valid tuned value for the next iteration */
        if (std::string(name) == "ATPERSON_BREAKER_THRESHOLD") setenv(name, "5", 1);
        if (std::string(name) == "ATPERSON_BREAKER_COOLDOWN") setenv(name, "60", 1);
        if (std::string(name) == "ATPERSON_BREAKER_MAX_COOLDOWN") setenv(name, "600", 1);
        if (std::string(name) == "ATPERSON_PROPOSAL_FAILURE_LIMIT") setenv(name, "4", 1);
        if (std::string(name) == "ATPERSON_SCHEDULER_MAX_PENDING") setenv(name, "7", 1);
    }
    for (const char *name : {"ATPERSON_BREAKER_THRESHOLD", "ATPERSON_BREAKER_COOLDOWN",
                             "ATPERSON_BREAKER_MAX_COOLDOWN", "ATPERSON_PROPOSAL_FAILURE_LIMIT",
                             "ATPERSON_SCHEDULER_MAX_PENDING"}) {
        unsetenv(name);
    }
}

/* Engagement consent (engagement.hpp): by default the entity only likes people
 * who engaged with it first, and never anyone on the do-not-engage list. */
void seed_like_context(Ledger &ledger) {
    std::uint64_t id = 0u;
    ledger.append("at://did:plc:author/app.bsky.feed.post/3kabc", "did:plc:author", NOW,
                  Ledger::digest("alpha"), 1u, ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id);
}

SchedulerConfig like_config(atperson::EngagementMode mode) {
    SchedulerConfig config = enabled_config();
    config.graduated_likes = true;
    config.engagement.mode = mode;
    return config;
}

void test_likes_require_an_invitation_by_default() {
    unsetenv("ATPERSON_DO_NOT_ENGAGE");
    const GateFiles gates("consent-invited");
    FakeWriter writer;
    LanguageGraph graph;
    Ledger ledger(gates.root / "ledger.bin");
    seed_like_context(ledger);

    /* A stranger's post is not liked. */
    SchedulerCycleReport report = atperson::run_scheduler_cycle(
        like_config(atperson::EngagementMode::Invited), make_cycle(gates, gates.root, writer),
        graph, ledger);
    assert(report.engagement_refused == 1u && report.last_engagement_refusal == "not_invited");
    assert(report.proposals_written == 0u);

    /* Once that author has replied to something the entity published, they
     * have engaged first, and the like goes through. */
    atperson::JournalEvent event;
    event.action_id = "3laction1";
    event.event_uri = "at://did:plc:author/app.bsky.feed.post/3kreply";
    event.author_did = "did:plc:author";
    event.via = "parent";
    event.at = "2026-09-17T00:00:00Z";
    atperson::append_journal_event(gates.journal, event);
    report = atperson::run_scheduler_cycle(like_config(atperson::EngagementMode::Invited),
                                           make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.engagement_refused == 0u && report.proposals_written == 1u);
    assert(report.graduated_likes_written == 1u);
}

void test_the_do_not_engage_list_wins_in_every_mode_and_is_live() {
    unsetenv("ATPERSON_DO_NOT_ENGAGE");
    const GateFiles gates("consent-optout");
    FakeWriter writer;
    LanguageGraph graph;
    Ledger ledger(gates.root / "ledger.bin");
    seed_like_context(ledger);
    write_file(gates.root / "do-not-engage.txt",
               "# no thanks\ndid:plc:author\nnot a did\n\n");

    SchedulerCycleReport report = atperson::run_scheduler_cycle(
        like_config(atperson::EngagementMode::Open), make_cycle(gates, gates.root, writer), graph,
        ledger);
    assert(report.engagement_refused == 1u && report.last_engagement_refusal == "opted_out");
    assert(report.proposals_written == 0u);
    assert(report.engagement_invalid_lines == 1u); /* the typo is counted, not fatal */

    /* Even an author who has engaged is refused once opted out. */
    atperson::JournalEvent event;
    event.action_id = "3laction1";
    event.event_uri = "at://did:plc:author/app.bsky.feed.post/3kreply";
    event.author_did = "did:plc:author";
    event.via = "parent";
    event.at = "2026-09-17T00:00:00Z";
    atperson::append_journal_event(gates.journal, event);
    report = atperson::run_scheduler_cycle(like_config(atperson::EngagementMode::Invited),
                                           make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.last_engagement_refusal == "opted_out" && report.proposals_written == 0u);

    /* Removing them from the list takes effect on the next cycle. */
    write_file(gates.root / "do-not-engage.txt", "");
    report = atperson::run_scheduler_cycle(like_config(atperson::EngagementMode::Invited),
                                           make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.proposals_written == 1u);
}

void test_a_frozen_like_is_rechecked_against_the_list_at_execution() {
    unsetenv("ATPERSON_DO_NOT_ENGAGE");
    const GateFiles gates("consent-exec");
    FakeWriter writer;
    LanguageGraph graph;
    Ledger ledger(gates.root / "ledger.bin");
    seed_like_context(ledger);
    const SchedulerConfig config = like_config(atperson::EngagementMode::Open);
    atperson::run_scheduler_cycle(config, make_cycle(gates, gates.root, writer), graph, ledger);
    std::filesystem::path proposal;
    for (const auto &entry : std::filesystem::directory_iterator(gates.root / "proposals")) {
        if (entry.path().extension() == ".json") {
            proposal = entry.path();
        }
    }
    const OutboundAction like = atperson::load_outbound_action(proposal);
    assert(like.kind == OutboundActionKind::Like);
    ControlState control = atperson::load_control_state(gates.control);
    control.approved_digests.push_back(like.digest);
    atperson::save_control_state(control, gates.control);

    /* Approved, but the operator has opted that person out since. */
    write_file(gates.root / "do-not-engage.txt", "did:plc:author\n");
    const SchedulerCycleReport report = atperson::run_scheduler_cycle(
        config, make_cycle(gates, gates.root, writer), graph, ledger);
    assert(report.engagement_refused >= 1u && report.last_engagement_refusal == "opted_out");
    assert(report.executions_attempted == 0u && writer.put_calls == 0);
    assert(std::filesystem::exists(proposal));
}

void test_engagement_mode_comes_from_the_environment() {
    unsetenv("ATPERSON_ENGAGEMENT");
    assert(atperson::scheduler_config_from_environment().engagement.mode ==
           atperson::EngagementMode::Invited);
    setenv("ATPERSON_ENGAGEMENT", "open", 1);
    assert(atperson::scheduler_config_from_environment().engagement.mode ==
           atperson::EngagementMode::Open);
    setenv("ATPERSON_ENGAGEMENT", "invited", 1);
    assert(atperson::scheduler_config_from_environment().engagement.mode ==
           atperson::EngagementMode::Invited);
    for (const char *bad : {"everyone", "Open", "1"}) {
        setenv("ATPERSON_ENGAGEMENT", bad, 1);
        bool threw = false;
        try {
            (void)atperson::scheduler_config_from_environment();
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
    unsetenv("ATPERSON_ENGAGEMENT");
}

void test_decision_valence_guard_comes_from_the_environment() {
    unsetenv("ATPERSON_DECISION_MIN_VALENCE");
    const atperson::SchedulerConfig off = atperson::scheduler_config_from_environment();
    assert(!off.decision.guards.valence_guard);

    setenv("ATPERSON_DECISION_MIN_VALENCE", "-0.5", 1);
    const atperson::SchedulerConfig on = atperson::scheduler_config_from_environment();
    assert(on.decision.guards.valence_guard);
    assert(on.decision.guards.min_valence == -0.5f);

    setenv("ATPERSON_DECISION_MIN_VALENCE", "0.5", 1);
    bool threw = false;
    try {
        (void)atperson::scheduler_config_from_environment();
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    unsetenv("ATPERSON_DECISION_MIN_VALENCE");
}

int main() {
    test_armed_entity_publishes_with_no_human_step();
    test_armed_entity_still_obeys_pause_and_disarm();
    test_output_guard_refuses_denied_text_and_recovers_when_lifted();
    test_output_guard_never_repeats_published_text();
    test_output_guard_rechecks_frozen_proposals_at_execution();
    test_output_guard_repeat_window_comes_from_the_environment();
    test_breaker_opens_on_repeated_failures_and_heals_itself();
    test_a_failed_probe_reopens_with_a_longer_cooldown();
    test_refusals_do_not_trip_the_breaker();
    test_a_poison_proposal_is_quarantined_and_never_reproposed();
    test_the_pending_queue_is_capped();
    test_breaker_settings_come_from_the_environment();
    test_likes_require_an_invitation_by_default();
    test_the_do_not_engage_list_wins_in_every_mode_and_is_live();
    test_a_frozen_like_is_rechecked_against_the_list_at_execution();
    test_engagement_mode_comes_from_the_environment();
    test_decision_valence_guard_comes_from_the_environment();
    test_disabled_scheduler_is_inert();
    test_decision_writes_proposal_but_never_executes_unapproved();
    test_approved_proposal_executes_and_is_consumed();
    test_pause_between_cycles_refuses_execution();
    test_existing_proposal_is_never_rewritten();
    test_abstention_writes_no_proposal();
    test_graduated_like_from_below_floor_abstention();
    test_graduated_like_requires_flag_and_likeable_source();
    test_drives_reorder_first_decision_context();
    test_intent_conversation_lifecycle();
    std::cout << "atperson-scheduler: all assertions passed\n";
    return 0;
}
