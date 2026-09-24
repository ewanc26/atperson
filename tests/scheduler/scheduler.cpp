/* Autonomous scheduler (#140): decision -> proposal -> approved execution,
 * composed through the existing gate chain, plus the pending-social-intent
 * (#150) conversation lifecycle — an executed post opens an intent, the
 * invited reply is composed as a continuation reply, and a closed window
 * expires the intent and resolves its action unmet. Offline: the network is a
 * fake OutboundWriter, the clock is injected, the ledger is real. */
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

int main() {
    test_disabled_scheduler_is_inert();
    test_decision_writes_proposal_but_never_executes_unapproved();
    test_approved_proposal_executes_and_is_consumed();
    test_pause_between_cycles_refuses_execution();
    test_existing_proposal_is_never_rewritten();
    test_abstention_writes_no_proposal();
    test_drives_reorder_first_decision_context();
    test_intent_conversation_lifecycle();
    std::cout << "atperson-scheduler: all assertions passed\n";
    return 0;
}
