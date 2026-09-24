/* Longitudinal self-evaluation metrics (#153): the bounded, read-only metric
 * snapshot module, its pass and its CLI atoms.
 *
 * Covers: metric snapshot round-trip / one-record-per-file layout /
 * id-file agreement / malformed refusal / torn-empty skip / previous-chain
 * provenance; the pass's schema-versioned first snapshot, cumulative actions
 * executed vs. admitted, terminal intent success per #150, cumulative reply
 * ratio, cumulative valence drift, familiarity growth from the ledger, the
 * bounded window trace, cadence gating (rerun within cadence writes nothing),
 * one-pass reflection of a fixture change (rating drop, reply spike), the
 * read-only no-training guarantee, and the `metrics` / `selfeval` CLI atoms.
 * Offline, no network; the clock is injected. */
#include "cli/metrics.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "journal/store.hpp"
#include "selfeval/config.hpp"
#include "selfeval/pass.hpp"
#include "selfeval/store.hpp"
#include "state/time.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atperson::IntentState;
using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::JournalExpectationState;
using atperson::JournalIntent;
using atperson::JournalResolution;
using atperson::LanguageGraph;
using atperson::MetricSnapshot;
using atperson::SelfEvalConfig;

constexpr std::int64_t NOW = 1'700'000'000;
constexpr std::int64_t CADENCE = 604'800;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-selfeval-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

SelfEvalConfig config() {
    SelfEvalConfig config;
    config.enabled = true;
    config.cadence_seconds = CADENCE;
    config.max_trace_authors = 8u;
    config.max_trace_groups = 8u;
    return config;
}

void append_action(const std::filesystem::path &path, std::string_view id,
                   JournalActionOutcome outcome, std::int64_t at) {
    JournalAction action;
    action.id = std::string(id);
    action.kind = "post";
    action.text = "fixture action";
    action.digest = "0123456789abcdef";
    action.outcome = outcome;
    action.reason = "fixture";
    action.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_action(path, action);
}

void append_event(const std::filesystem::path &path, std::string_view via, std::int64_t at) {
    atperson::JournalEvent entry;
    entry.action_id = "3fixture00000000";
    entry.event_uri = "at://selfeval/event/" + std::to_string(at);
    entry.author_did = "did:plc:other";
    entry.via = std::string(via);
    entry.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_event(path, entry);
}

void append_valence(const std::filesystem::path &path, std::string_view token,
                    std::string_view kind, float signal, std::int64_t at) {
    atperson::JournalValence entry;
    entry.token = std::string(token);
    entry.kind = std::string(kind);
    entry.signal = signal;
    entry.at_epoch = static_cast<std::uint64_t>(at);
    entry.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_valence(path, entry);
}

void append_resolution(const std::filesystem::path &path, std::string_view state,
                       std::int64_t at) {
    JournalResolution entry;
    entry.action_id = "3fixture00000000";
    entry.state = *atperson::journal_expectation_state_from_name(state);
    entry.at_epoch = static_cast<std::uint64_t>(at);
    entry.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_resolution(path, entry);
}

void append_intent(const std::filesystem::path &path, std::string_view id,
                   std::string_view state, std::int64_t at) {
    JournalIntent entry;
    entry.id = std::string(id);
    entry.actions = {"3fixture00000000"};
    entry.responder = "anyone";
    entry.expires_at_epoch = static_cast<std::uint64_t>(at) + 100u;
    entry.expires_at = atperson::rfc3339_from_unix(at + 100);
    entry.max_continuations = 3u;
    entry.state = *atperson::intent_state_from_name(state);
    entry.at_epoch = static_cast<std::uint64_t>(at);
    entry.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_intent(path, entry);
}

void mirror_learned(LanguageGraph &graph, std::string_view author, std::uint64_t observed_at,
                    std::uint64_t seed) {
    atp_ledger_entry entry{};
    entry.observed_at = observed_at;
    entry.content_digest = 0x12345678u | (seed << 8u);
    entry.schema_version = 1u;
    entry.outcome = ATP_LEDGER_OUTCOME_LEARNED;
    std::strncpy(entry.source_id, "at://selfeval/ledger", sizeof(entry.source_id) - 1u);
    std::strncpy(entry.author_did, author.data(), sizeof(entry.author_did) - 1u);
    graph.record_ledger_entry(entry);
}

bool float_eq(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

/* A journal shaped for metric assertions: 4 actions (2 executed), 4 linked
 * events (1 reply), 3 valence updates on 2 tokens, 4 intents (1 closed,
 * 1 expired, 2 open), 2 resolutions. */
void shape_journal(const std::filesystem::path &path, std::int64_t at) {
    append_action(path, "3fixture00000001", JournalActionOutcome::Executed, at);
    append_action(path, "3fixture00000002", JournalActionOutcome::Executed, at - 1);
    append_action(path, "3fixture00000003", JournalActionOutcome::Denied, at - 2);
    append_action(path, "3fixture00000004", JournalActionOutcome::Deferred, at - 3);
    append_event(path, "post", at - 4);
    append_event(path, "reply", at - 5);
    append_event(path, "quote", at - 6);
    append_event(path, "post", at - 7);
    append_valence(path, "star", "interaction", 0.5f, at - 1);
    append_valence(path, "star", "interaction", 0.25f, at - 2);
    append_valence(path, "void", "approach", -1.0f, at - 3);
    append_intent(path, "at://selfeval/thread/one", "open", at - 1);
    append_intent(path, "at://selfeval/thread/two", "open", at - 2);
    append_intent(path, "at://selfeval/thread/two", "closed", at - 3);
    append_intent(path, "at://selfeval/thread/three", "open", at - 4);
    append_intent(path, "at://selfeval/thread/three", "expired", at - 5);
    append_intent(path, "at://selfeval/thread/four", "open", at - 6);
    append_resolution(path, "met", at - 1);
    append_resolution(path, "expired", at - 2);
}

/* --- store ------------------------------------------------------------- */

int test_store_round_trip() {
    const auto dir = scratch_dir("store");
    const auto store = dir / "metrics";

    MetricSnapshot snapshot;
    snapshot.id = "2nd3rd4rd5rd6rd7rd8rd9";
    snapshot.at = atperson::rfc3339_from_unix(NOW);
    snapshot.period_start = atperson::rfc3339_from_unix(NOW - CADENCE);
    snapshot.period_end = snapshot.at;
    snapshot.cadence_seconds = static_cast<std::uint64_t>(CADENCE);
    snapshot.actions.attempts = 4u;
    snapshot.actions.executed = 2u;
    snapshot.actions.denied = 1u;
    snapshot.actions.deferred = 1u;
    snapshot.actions.failed = 0u;
    snapshot.actions.success_rate = 0.5;
    snapshot.interaction.invites = 4u;
    snapshot.interaction.invites_replied = 1u;
    snapshot.interaction.invites_expired = 1u;
    snapshot.interaction.invites_pending = 2u;
    snapshot.interaction.success_rate = 0.5;
    snapshot.reply_ratio.events = 4u;
    snapshot.reply_ratio.replies = 1u;
    snapshot.reply_ratio.ratio = 0.25;
    snapshot.valence.updates = 3u;
    snapshot.valence.tokens = 2u;
    snapshot.valence.drift = -0.25;
    snapshot.familiarity.authors = 2u;
    snapshot.familiarity.encounters = 3u;
    snapshot.familiarity.new_authors = 1u;
    snapshot.familiarity.accretion = 0.5;
    snapshot.trace.episodes = 2u;
    snapshot.trace.events = 3u;
    snapshot.trace.resolutions = 1u;
    snapshot.trace.valence_updates = 3u;
    snapshot.trace.new_authors = {"did:plc:alice"};
    atperson::MetricGroup group;
    group.token = "void";
    group.kind = "approach";
    group.signal_sum = -1.0;
    group.count = 1u;
    snapshot.trace.top_valence = {group};
    snapshot.has_previous = true;
    snapshot.previous_id = "2nd3rd4rd5rd6rd7rd8rd8";
    snapshot.previous_at = atperson::rfc3339_from_unix(NOW - CADENCE);

    atperson::write_metric_snapshot(store, snapshot);
    if (!std::filesystem::exists(store / "2nd3rd4rd5rd6rd7rd8rd9.json")) {
        std::cerr << "FAIL snapshot one-record-per-file layout\n";
        return 1;
    }

    const std::vector<MetricSnapshot> loaded = atperson::load_metric_snapshots(store);
    if (loaded.size() != 1u) {
        std::cerr << "FAIL snapshot round-trip count\n";
        return 1;
    }
    const MetricSnapshot &back = loaded[0];
    if (back.id != snapshot.id || back.period_start != snapshot.period_start ||
        back.period_end != snapshot.period_end ||
        back.cadence_seconds != snapshot.cadence_seconds) {
        std::cerr << "FAIL snapshot id/period round-trip\n";
        return 1;
    }
    if (back.actions.attempts != 4u || back.actions.executed != 2u ||
        !float_eq(back.actions.success_rate, 0.5) || back.actions.denied != 1u ||
        back.actions.deferred != 1u || back.actions.failed != 0u ||
        back.actions.dry_run != 0u) {
        std::cerr << "FAIL snapshot actions round-trip\n";
        return 1;
    }
    if (back.interaction.invites != 4u || back.interaction.invites_replied != 1u ||
        back.interaction.invites_expired != 1u || back.interaction.invites_pending != 2u ||
        !float_eq(back.interaction.success_rate, 0.5)) {
        std::cerr << "FAIL snapshot interaction round-trip\n";
        return 1;
    }
    if (back.reply_ratio.events != 4u || back.reply_ratio.replies != 1u ||
        !float_eq(back.reply_ratio.ratio, 0.25)) {
        std::cerr << "FAIL snapshot reply_ratio round-trip\n";
        return 1;
    }
    if (back.valence.updates != 3u || back.valence.tokens != 2u ||
        !float_eq(back.valence.drift, -0.25)) {
        std::cerr << "FAIL snapshot valence round-trip\n";
        return 1;
    }
    if (back.familiarity.authors != 2u || back.familiarity.encounters != 3u ||
        back.familiarity.new_authors != 1u || !float_eq(back.familiarity.accretion, 0.5)) {
        std::cerr << "FAIL snapshot familiarity round-trip\n";
        return 1;
    }
    if (back.trace.episodes != 2u || back.trace.events != 3u ||
        back.trace.resolutions != 1u || back.trace.valence_updates != 3u ||
        back.trace.new_authors.size() != 1u || back.trace.new_authors[0] != "did:plc:alice" ||
        back.trace.top_valence.size() != 1u) {
        std::cerr << "FAIL snapshot trace round-trip\n";
        return 1;
    }
    if (!back.has_previous || back.previous_id != snapshot.previous_id ||
        back.previous_at != snapshot.previous_at) {
        std::cerr << "FAIL snapshot previous-chain round-trip\n";
        return 1;
    }
    return 0;
}

int test_store_torn_and_mismatch() {
    const auto dir = scratch_dir("torn");
    const auto store = dir / "metrics";

    MetricSnapshot snapshot;
    snapshot.id = "2nd3rd4rd5rd6rd7rd8rd9";
    snapshot.at = atperson::rfc3339_from_unix(NOW);
    snapshot.period_start = snapshot.at;
    snapshot.period_end = snapshot.at;
    snapshot.cadence_seconds = 604'800;
    atperson::write_metric_snapshot(store, snapshot);

    /* A torn (empty) file is skipped, not fatal. */
    {
        std::ofstream torn(store / "2nd3rd4rd5rd6rd7rd8rd9a.json", std::ios::trunc);
        torn.flush();
    }
    /* A stray non-json file is ignored. */
    {
        std::ofstream stray(store / "notes.txt", std::ios::trunc);
        stray << "not a snapshot\n";
    }
    const std::vector<MetricSnapshot> loaded = atperson::load_metric_snapshots(store);
    if (loaded.size() != 1u) {
        std::cerr << "FAIL torn/stray files interfered\n";
        return 1;
    }

    /* An id/stem mismatch is corruption: rejected. */
    {
        MetricSnapshot mismatch = snapshot;
        mismatch.id = "2nd3rd4rd5rd6rd7rd8rd9b";
        const std::filesystem::path other = store / "2nd3rd4rd5rd6rd7rd8rd9c.json";
        std::ofstream output(other, std::ios::binary | std::ios::trunc);
        output << atperson::serialise_metric_snapshot(mismatch);
        output.flush();
        bool threw = false;
        try {
            (void)atperson::load_metric_snapshots(store);
        } catch (const atperson::MetricError &) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "FAIL id/stem mismatch not rejected\n";
            return 1;
        }
    }
    return 0;
}

/* --- pass -------------------------------------------------------------- */

int test_pass_first_snapshot_schema() {
    const auto dir = scratch_dir("first");
    const auto store = dir / "metrics";

    LanguageGraph graph;
    const atperson::SelfEvalReport report = atperson::run_self_eval_pass(
        store, atperson::JournalContents(), graph, config(), static_cast<std::uint64_t>(NOW));
    if (!report.due || report.reason != "no prior snapshot" || !report.snapshot.has_value() ||
        report.prior_snapshots != 0u) {
        std::cerr << "FAIL first snapshot due\n";
        return 1;
    }
    const MetricSnapshot &snapshot = *report.snapshot;
    if (snapshot.at != atperson::rfc3339_from_unix(NOW) ||
        snapshot.period_end != snapshot.at ||
        snapshot.period_start != atperson::rfc3339_from_unix(NOW - CADENCE) ||
        snapshot.cadence_seconds != static_cast<std::uint64_t>(CADENCE) ||
        snapshot.has_previous) {
        std::cerr << "FAIL first snapshot period/schema\n";
        return 1;
    }
    /* The stored record round-trips as "atperson-metrics" v1 with a clean
     * format-versioned envelope. */
    const std::vector<MetricSnapshot> stored = atperson::load_metric_snapshots(store);
    if (stored.size() != 1u || stored[0].id != snapshot.id) {
        std::cerr << "FAIL first snapshot persisted\n";
        return 1;
    }
    const std::string json = atperson::serialise_metric_snapshot(stored[0]);
    if (json.find("\"format\":\"atperson-metrics\"") == std::string::npos ||
        json.find("\"version\":1") == std::string::npos) {
        std::cerr << "FAIL snapshot lacks schema-versioned envelope\n";
        return 1;
    }
    if (snapshot.actions.attempts != 0u || snapshot.reply_ratio.events != 0u ||
        snapshot.valence.updates != 0u || snapshot.familiarity.authors != 0u ||
        snapshot.trace.episodes != 0u) {
        std::cerr << "FAIL empty-state snapshot baseline\n";
        return 1;
    }
    return 0;
}

int test_pass_metrics_from_fixture() {
    const auto dir = scratch_dir("metrics");
    const auto store = dir / "metrics";
    const auto journal_path = dir / "action-journal.jsonl";
    shape_journal(journal_path, NOW);

    LanguageGraph graph;
    mirror_learned(graph, "did:plc:alice", static_cast<std::uint64_t>(NOW), 1u);
    mirror_learned(graph, "did:plc:bob", static_cast<std::uint64_t>(NOW - 5), 2u);
    graph.remember("alpha beta", "at://selfeval/a", "did:plc:alice", static_cast<std::uint64_t>(NOW),
                   12345u, 1u, 500u);

    const atperson::SelfEvalReport report = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(NOW));
    if (!report.due) {
        std::cerr << "FAIL fixture pass not due\n";
        return 1;
    }
    const MetricSnapshot &snapshot = *report.snapshot;

    /* actions: 4 attempts, 2 executed, 1 denied, 1 deferred. */
    if (snapshot.actions.attempts != 4u || snapshot.actions.executed != 2u ||
        snapshot.actions.denied != 1u || snapshot.actions.deferred != 1u ||
        !float_eq(snapshot.actions.success_rate, 0.5)) {
        std::cerr << "FAIL actions metrics\n";
        return 1;
    }
    /* interaction: 4 threads, last-state 1 closed + 1 expired + 2 open. */
    if (snapshot.interaction.invites != 4u || snapshot.interaction.invites_replied != 1u ||
        snapshot.interaction.invites_expired != 1u ||
        snapshot.interaction.invites_pending != 2u ||
        !float_eq(snapshot.interaction.success_rate, 0.5)) {
        std::cerr << "FAIL interaction metrics\n";
        return 1;
    }
    /* reply ratio: 1 reply over 4 events. */
    if (snapshot.reply_ratio.events != 4u || snapshot.reply_ratio.replies != 1u ||
        !float_eq(snapshot.reply_ratio.ratio, 0.25)) {
        std::cerr << "FAIL reply_ratio metrics\n";
        return 1;
    }
    /* valence: 0.5 + 0.25 - 1.0 drift over 2 tokens. */
    if (snapshot.valence.updates != 3u || snapshot.valence.tokens != 2u ||
        !float_eq(snapshot.valence.drift, -0.25)) {
        std::cerr << "FAIL valence metrics\n";
        return 1;
    }
    /* familiarity: two distinct ledger authors, both observed inside the
     * period (alice at NOW, bob NOW-5), so both are new here. */
    if (snapshot.familiarity.authors != 2u || snapshot.familiarity.encounters != 2u ||
        snapshot.familiarity.new_authors != 2u || !float_eq(snapshot.familiarity.accretion, 1.0)) {
        std::cerr << "FAIL familiarity metrics\n";
        return 1;
    }
    /* trace: 1 episode, 4 events, 2 resolutions, 3 valence updates. */
    if (snapshot.trace.episodes != 1u || snapshot.trace.events != 4u ||
        snapshot.trace.resolutions != 2u || snapshot.trace.valence_updates != 3u) {
        std::cerr << "FAIL window trace\n";
        return 1;
    }
    if (snapshot.trace.new_authors.size() != 2u) {
        std::cerr << "FAIL trace new authors\n";
        return 1;
    }
    /* top valence: the void approach (-1.0) dominates. */
    if (snapshot.trace.top_valence.size() != 2u ||
        snapshot.trace.top_valence[0].token != "void" ||
        !float_eq(snapshot.trace.top_valence[0].signal_sum, -1.0)) {
        std::cerr << "FAIL trace top valence\n";
        return 1;
    }
    return 0;
}

int test_pass_cadence_gate() {
    const auto dir = scratch_dir("cadence");
    const auto store = dir / "metrics";
    const auto journal_path = dir / "action-journal.jsonl";
    shape_journal(journal_path, NOW);

    LanguageGraph graph;
    const atperson::SelfEvalReport first = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(NOW));
    if (!first.due || first.prior_snapshots != 0u) {
        std::cerr << "FAIL cadence first due\n";
        return 1;
    }

    /* Same clock: deterministic rerun writes nothing. */
    const atperson::SelfEvalReport rerun = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(NOW));
    if (rerun.due || rerun.reason != "inside cadence" || rerun.prior_snapshots != 1u) {
        std::cerr << "FAIL cadence rerun gated\n";
        return 1;
    }
    if (atperson::load_metric_snapshots(store).size() != 1u) {
        std::cerr << "FAIL cadence rerun wrote a snapshot\n";
        return 1;
    }

    /* One cadence later: due with the previous chain. */
    const std::int64_t later = NOW + CADENCE;
    shape_journal(journal_path, later);
    const atperson::SelfEvalReport second = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(later));
    if (!second.due || second.reason != "cadence elapsed" || second.prior_snapshots != 1u ||
        !second.snapshot.has_value()) {
        std::cerr << "FAIL cadence next due\n";
        return 1;
    }
    const MetricSnapshot &second_snapshot = *second.snapshot;
    if (!second_snapshot.has_previous ||
        second_snapshot.previous_id != first.snapshot->id ||
        second_snapshot.previous_at != first.snapshot->at) {
        std::cerr << "FAIL cadence previous chain\n";
        return 1;
    }
    if (atperson::load_metric_snapshots(store).size() != 2u) {
        std::cerr << "FAIL second snapshot persisted count\n";
        return 1;
    }
    return 0;
}

int test_pass_reflects_fixture_change() {
    const auto dir = scratch_dir("change");
    const auto store = dir / "metrics";
    const auto journal_path = dir / "action-journal.jsonl";
    shape_journal(journal_path, NOW);

    LanguageGraph graph;
    (void)atperson::run_self_eval_pass(store, atperson::load_journal(journal_path), graph,
                                       config(), static_cast<std::uint64_t>(NOW));

    /* Fixture change right after the first snapshot: four reply events
     * (5 replies over 8 events) and one rating drop. One cadence later the
     * relevant metrics reflect it within a single pass. */
    const std::int64_t later = NOW + CADENCE;
    append_event(journal_path, "reply", later - 1);
    append_event(journal_path, "reply", later - 2);
    append_event(journal_path, "reply", later - 3);
    append_event(journal_path, "reply", later - 4);
    append_valence(journal_path, "star", "interaction", -0.5f, later - 1);
    append_resolution(journal_path, "met", later - 1);

    const atperson::SelfEvalReport second = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(later));
    if (!second.due || !second.snapshot.has_value()) {
        std::cerr << "FAIL change pass not due\n";
        return 1;
    }
    const MetricSnapshot &snapshot = *second.snapshot;
    if (snapshot.reply_ratio.events != 8u || snapshot.reply_ratio.replies != 5u ||
        !float_eq(snapshot.reply_ratio.ratio, 0.625)) {
        std::cerr << "FAIL reply spike not reflected in one pass\n";
        return 1;
    }
    /* valence: -0.25 then -0.5 -> -0.75. */
    if (!float_eq(snapshot.valence.drift, -0.75)) {
        std::cerr << "FAIL rating drop not reflected in one pass\n";
        return 1;
    }
    if (snapshot.actions.executed != 2u || snapshot.actions.attempts != 4u ||
        !float_eq(snapshot.actions.success_rate, 0.5)) {
        std::cerr << "FAIL unchanged actions drifted\n";
        return 1;
    }
    return 0;
}

int test_pass_read_only() {
    const auto dir = scratch_dir("readonly");
    const auto store = dir / "metrics";
    const auto journal_path = dir / "action-journal.jsonl";
    shape_journal(journal_path, NOW);
    const std::string journal_before = [&]() {
        std::ifstream file(journal_path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    }();

    LanguageGraph graph;
    graph.observe("alpha beta gamma", "at://selfeval/observe");
    const std::size_t episodes_before = graph.episodes().size();
    const std::size_t ledger_before = graph.ledger_entries().size();
    const auto stats_before = graph.stats();

    const atperson::SelfEvalReport report = atperson::run_self_eval_pass(
        store, atperson::load_journal(journal_path), graph, config(),
        static_cast<std::uint64_t>(NOW));
    if (!report.due) {
        std::cerr << "FAIL read-only pass not due\n";
        return 1;
    }
    if (graph.episodes().size() != episodes_before ||
        graph.ledger_entries().size() != ledger_before ||
        graph.stats().observations != stats_before.observations ||
        graph.stats().training_steps != stats_before.training_steps) {
        std::cerr << "FAIL pass mutated learned state\n";
        return 1;
    }
    {
        std::ifstream file(journal_path, std::ios::binary);
        const std::string after{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};
        if (after != journal_before) {
            std::cerr << "FAIL pass modified the journal\n";
            return 1;
        }
    }
    /* The only new artifact is the snapshot, and it is versioned. */
    std::ifstream snapshot_file(store / (report.snapshot->id + ".json"), std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(snapshot_file),
                           std::istreambuf_iterator<char>()};
    if (json.find("\"version\":1") == std::string::npos) {
        std::cerr << "FAIL read-only snapshot unversioned\n";
        return 1;
    }
    return 0;
}

int test_store_rejects_overwrite() {
    const auto dir = scratch_dir("overwrite");
    const auto store = dir / "metrics";
    MetricSnapshot snapshot;
    snapshot.id = "2nd3rd4rd5rd6rd7rd8rd9";
    snapshot.at = atperson::rfc3339_from_unix(NOW);
    snapshot.period_start = snapshot.at;
    snapshot.period_end = snapshot.at;
    snapshot.cadence_seconds = 604'800;
    atperson::write_metric_snapshot(store, snapshot);
    bool threw = false;
    try {
        atperson::write_metric_snapshot(store, snapshot);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "FAIL duplicate snapshot overwrite not refused\n";
        return 1;
    }
    return 0;
}

/* --- CLI --------------------------------------------------------------- */

int test_cli_surface() {
    const auto dir = scratch_dir("cli");
    const auto journal_path = dir / "action-journal.jsonl";
    shape_journal(journal_path, NOW);

    LanguageGraph graph;
    std::ostringstream eval;
    if (atperson::cli::run_self_eval_command(eval, dir, journal_path, graph, NOW,
                                             atperson::rfc3339_from_unix(NOW)) != 0) {
        std::cerr << "FAIL selfeval command\n";
        return 1;
    }
    if (eval.str().find("self-eval: snapshot due") == std::string::npos ||
        eval.str().find("actions") == std::string::npos ||
        eval.str().find("reply_ratio") == std::string::npos) {
        std::cerr << "FAIL selfeval command output '" << eval.str() << "'\n";
        return 1;
    }

    std::ostringstream list;
    const std::string_view no_args[] = {};
    if (atperson::cli::run_metrics_list(list, dir, no_args, 0u) != 0) {
        std::cerr << "FAIL metrics list command\n";
        return 1;
    }
    const std::string listed = list.str();
    if (listed.find("metrics: format=atperson-metrics version=1 snapshots=1") ==
            std::string::npos ||
        listed.find("first-snapshot") == std::string::npos ||
        listed.find("span=") == std::string::npos || listed.find("trace") == std::string::npos) {
        std::cerr << "FAIL metrics list output '" << listed << "'\n";
        return 1;
    }

    /* A second pass a cadence later produces two rows and a delta line. */
    const std::int64_t later = NOW + CADENCE;
    append_event(journal_path, "reply", later - 1);
    append_event(journal_path, "reply", later - 2);
    std::ostringstream eval2;
    if (atperson::cli::run_self_eval_command(eval2, dir, journal_path, graph, later,
                                             atperson::rfc3339_from_unix(later)) != 0) {
        std::cerr << "FAIL selfeval second command\n";
        return 1;
    }
    std::ostringstream list2;
    if (atperson::cli::run_metrics_list(list2, dir, no_args, 0u) != 0) {
        std::cerr << "FAIL metrics list second command\n";
        return 1;
    }
    const std::string listed2 = list2.str();
    if (listed2.find("snapshots=2") == std::string::npos ||
        listed2.find("delta") == std::string::npos) {
        std::cerr << "FAIL metrics list trend deltas '" << listed2 << "'\n";
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    struct Case {
        const char *name;
        int (*run)();
    };
    const Case cases[] = {
        {"store-round-trip", test_store_round_trip},
        {"store-torn-mismatch", test_store_torn_and_mismatch},
        {"store-rejects-overwrite", test_store_rejects_overwrite},
        {"pass-first-snapshot-schema", test_pass_first_snapshot_schema},
        {"pass-metrics-from-fixture", test_pass_metrics_from_fixture},
        {"pass-cadence-gate", test_pass_cadence_gate},
        {"pass-reflects-fixture-change", test_pass_reflects_fixture_change},
        {"pass-read-only", test_pass_read_only},
        {"cli-surface", test_cli_surface},
    };
    int failed = 0;
    for (const Case &test : cases) {
        if (test.run() != 0) {
            std::cerr << "FAILED " << test.name << '\n';
            ++failed;
        }
    }
    if (failed != 0) {
        std::cerr << failed << " self-eval test section(s) failed\n";
        return 1;
    }
    std::cout << "self-eval tests: " << (sizeof(cases) / sizeof(cases[0])) << " sections ok\n";
    return 0;
}