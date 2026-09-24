/* Autonomous scheduler (#140): decision -> proposal -> approved execution,
 * composed through the existing gate chain. Offline: the network is a fake
 * OutboundWriter, the clock is injected, the ledger is real. */
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
using atperson::JournalContents;
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
                          OutboundWriter &writer) {
    return SchedulerCycle{
        data_dir,
        data_dir / "proposals",
        atperson::OutboundAttemptPaths{gates.policy, gates.budget, gates.control, gates.audit,
                                        gates.journal, gates.envelopes},
        [&writer]() -> OutboundWriter & { return writer; },
        NOW,
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

} // namespace

int main() {
    test_disabled_scheduler_is_inert();
    test_decision_writes_proposal_but_never_executes_unapproved();
    test_approved_proposal_executes_and_is_consumed();
    test_pause_between_cycles_refuses_execution();
    test_existing_proposal_is_never_rewritten();
    test_abstention_writes_no_proposal();
    test_drives_reorder_first_decision_context();
    std::cout << "atperson-scheduler: all assertions passed\n";
    return 0;
}
