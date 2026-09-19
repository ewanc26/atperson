/* Outcome-to-valence mapping tests (#56): rule-table parsing, first-match-
 * wins ordering, unmapped outcomes, within-seconds windows, idempotency
 * across restarts, and the denied-outcome-scores-negative contract. The
 * `map` runner is exercised through the real command surface with a real
 * graph and journal files, so the graph mutation, journal append and
 * dedup semantics are covered end to end. Offline, no network. */

#include "journal/command.hpp"
#include "journal/rules.hpp"

#include "atperson/graph.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::JournalError;
using atperson::JournalEvent;
using atperson::JournalValence;
using atperson::LanguageGraph;
using atperson::journal::RuleTable;
using atperson::journal::ValenceRule;
using atperson::journal::first_matching_rule;
using atperson::journal::load_rule_table;
using atperson::journal::parse_rule_table;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-journal-rules-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void write_file(const std::filesystem::path &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

JournalAction action_with(JournalActionOutcome outcome, const std::string &id,
                          const std::string &text, const std::string &at) {
    JournalAction action;
    action.id = id;
    action.kind = "post";
    action.text = text;
    action.digest = "0123456789abcdef";
    action.outcome = outcome;
    action.reason = "allow";
    action.uri = "at://did:plc:example/app.bsky.feed.post/" + id;
    action.cid = "bafyreiabc123";
    action.at = at;
    return action;
}

const char *kSimpleRules = R"json({
  "format": "atperson-valence-rules",
  "version": 1,
  "rules": [
    {"id": "denied-negative", "when": {"outcome": "denied"},
     "kind": "action", "signal": -0.5},
    {"id": "replied-positive", "when": {"outcome": "executed", "min_events": 1,
     "within_seconds": 86400},
     "kind": "interaction", "signal": 0.5}
  ]
})json";

void test_parse_rejects_malformed_tables() {
    bool threw = false;
    try {
        (void)parse_rule_table("not json");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        (void)parse_rule_table(R"({"format": "something-else", "version": 1, "rules": []})");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        (void)parse_rule_table(R"({"format": "atperson-valence-rules", "version": 2, "rules": []})");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        (void)parse_rule_table(R"({"format": "atperson-valence-rules", "version": 1, "rules": [
            {"id": "a", "when": {"outcome": "exploded"}, "kind": "action", "signal": 0.1}
        ]})");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        (void)parse_rule_table(R"({"format": "atperson-valence-rules", "version": 1, "rules": [
            {"id": "a", "when": {"outcome": "denied"}, "kind": "action", "signal": 2.0}
        ]})");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        (void)parse_rule_table(R"({"format": "atperson-valence-rules", "version": 1, "rules": [
            {"id": "a", "when": {"outcome": "denied"}, "kind": "action", "signal": 0.1},
            {"id": "a", "when": {"outcome": "failed"}, "kind": "action", "signal": -0.1}
        ]})");
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);
}

void test_parse_and_match() {
    const RuleTable table = parse_rule_table(kSimpleRules);
    assert(table.rules.size() == 2u);
    assert(table.rules[0].id == "denied-negative");
    assert(table.rules[0].outcome == JournalActionOutcome::Denied);
    assert(table.rules[0].kind == ATP_VALENCE_ACTION);
    assert(table.rules[0].signal == -0.5f);
    assert(!table.rules[0].min_events.has_value());
    assert(table.rules[1].id == "replied-positive");
    assert(table.rules[1].min_events.value() == 1u);
    assert(table.rules[1].within_seconds.value() == 86400u);

    /* First match wins: a rule earlier in the table shadows a later one
     * for the same outcome. */
    const RuleTable ordered = parse_rule_table(R"({
      "format": "atperson-valence-rules", "version": 1, "rules": [
        {"id": "first", "when": {"outcome": "denied"}, "kind": "action", "signal": -0.9},
        {"id": "second", "when": {"outcome": "denied"}, "kind": "avoid", "signal": -0.1}
      ]})");
    const JournalAction denied = action_with(JournalActionOutcome::Denied, "aaa", "hello",
                                             "2026-09-17T10:00:00Z");
    const ValenceRule *match = first_matching_rule(ordered, denied, {});
    assert(match != nullptr && match->id == "first");

    /* Unmapped outcome: no rule fires. */
    const JournalAction deferred = action_with(JournalActionOutcome::Deferred, "bbb", "hello",
                                                "2026-09-17T10:00:00Z");
    assert(first_matching_rule(ordered, deferred, {}) == nullptr);

    /* Empty table derives nothing. */
    const RuleTable empty = parse_rule_table(
        R"({"format": "atperson-valence-rules", "version": 1, "rules": []})");
    assert(first_matching_rule(empty, denied, {}) == nullptr);
}

void test_event_window_matching() {
    const RuleTable table = parse_rule_table(kSimpleRules);
    const JournalAction executed =
        action_with(JournalActionOutcome::Executed, "ccc", "moonlight", "2026-09-17T10:00:00Z");

    JournalEvent reply;
    reply.action_id = "ccc";
    reply.event_uri = "at://did:plc:other/app.bsky.feed.post/reply1";
    reply.author_did = "did:plc:other";
    reply.via = "parent";
    reply.at = "2026-09-17T22:00:00Z"; /* 12h later: inside the 24h window */

    const std::vector<JournalEvent> inside{reply};
    const ValenceRule *match = first_matching_rule(table, executed, inside);
    assert(match != nullptr && match->id == "replied-positive");

    JournalEvent late = reply;
    late.at = "2026-09-19T10:00:00Z"; /* 48h later: outside */
    const std::vector<JournalEvent> outside{late};
    assert(first_matching_rule(table, executed, outside) == nullptr);

    /* No events at all: the min_events trigger fails. */
    assert(first_matching_rule(table, executed, {}) == nullptr);
}

void test_map_command_end_to_end() {
    const auto root = scratch_dir("map");
    const auto journal_path = root / "action-journal.jsonl";
    const auto model_path = root / "model.bin";
    const auto rules_path = root / "rules.json";
    write_file(rules_path, kSimpleRules);

    /* A graph that has observed the denied action's tokens, so valence
     * attaches to experienced subjects. */
    LanguageGraph graph;
    graph.observe("the moon is a loyal companion", "local:fixture");

    /* Denied action with known tokens, deferred action with known tokens
     * (unmapped), executed action with a reply inside the window. */
    atperson::append_journal_action(
        journal_path, action_with(JournalActionOutcome::Denied, "act1",
                                  "the moon is a loyal companion", "2026-09-17T09:00:00Z"));
    atperson::append_journal_action(
        journal_path, action_with(JournalActionOutcome::Deferred, "act2",
                                  "the moon is a loyal companion", "2026-09-17T09:30:00Z"));
    atperson::append_journal_action(
        journal_path, action_with(JournalActionOutcome::Executed, "act3",
                                  "the moon is a loyal companion", "2026-09-17T10:00:00Z"));
    JournalEvent reply;
    reply.action_id = "act3";
    reply.event_uri = "at://did:plc:other/app.bsky.feed.post/reply1";
    reply.author_did = "did:plc:other";
    reply.via = "parent";
    reply.at = "2026-09-17T12:00:00Z";
    atperson::append_journal_event(journal_path, reply);

    std::ostringstream out;
    const std::string rule_file(rules_path.string());
    const std::string_view map_args[]{rule_file};
    const int status = atperson::journal::run_journal_command(
        out, graph, journal_path, root, model_path, "map",
        map_args, 1u, 1789600000, "2026-09-17T20:00:00Z");
    (void)status;

    /* Every distinct token of the denied action and the executed action
     * received one event; the deferred action mapped nothing. The denied
     * outcome scored negative action valence. */
    const JournalContents after = atperson::load_journal(journal_path);
    std::size_t denied_entries = 0u;
    std::size_t replied_entries = 0u;
    for (const JournalValence &entry : after.valence) {
        assert(entry.provenance.rfind("map:", 0u) == 0u);
        if (entry.source == "act1") {
            assert(entry.kind == "action");
            assert(entry.signal == -0.5f);
            ++denied_entries;
        } else if (entry.source == "act3") {
            assert(entry.kind == "interaction");
            assert(entry.signal == 0.5f);
            ++replied_entries;
        } else {
            assert(!"valence entry from an unexpected source");
        }
    }
    assert(denied_entries > 0u);
    assert(replied_entries > 0u);

    /* Valence state was applied to the graph and is inspectable. */
    const auto moon = graph.valence("moon");
    assert(moon.has_value());
    assert(moon->negative_events >= 1u);

    /* Idempotency: running map again derives nothing new. */
    std::ostringstream second_out;
    atperson::journal::run_journal_command(second_out, graph, journal_path, root, model_path,
                                           "map", map_args, 1u, 1789600100,
                                           "2026-09-17T20:01:40Z");
    const JournalContents twice = atperson::load_journal(journal_path);
    assert(twice.valence.size() == after.valence.size());

    /* A rebuild reproduces the same valence state from the journal alone:
     * the map-produced entries replay like any other valence entry. The
     * real rebuild replays the ledger first (creating the vocabulary),
     * then the valence log — mirror that order here. */
    LanguageGraph rebuilt;
    rebuilt.observe("the moon is a loyal companion", "local:rebuild");
    for (const JournalValence &entry : twice.valence) {
        const auto kind = atperson::valence_kind_from_name(entry.kind);
        assert(kind.has_value());
        rebuilt.valence_event(entry.token, kind.value(), entry.signal, entry.at_epoch,
                              entry.source);
    }
    const auto moon_rebuilt = rebuilt.valence("moon");
    assert(moon_rebuilt.has_value());
    assert(moon_rebuilt->event_count == moon->event_count);
}

void test_map_skips_unknown_tokens() {
    const auto root = scratch_dir("unknown");
    const auto journal_path = root / "action-journal.jsonl";
    const auto model_path = root / "model.bin";
    const auto rules_path = root / "rules.json";
    write_file(rules_path, kSimpleRules);

    /* Empty graph: no token has ever been observed, so no valence event
     * may attach — the mapping must not intern vocabulary. */
    LanguageGraph graph;
    atperson::append_journal_action(
        journal_path, action_with(JournalActionOutcome::Denied, "act1", "unseen words here",
                                  "2026-09-17T09:00:00Z"));

    std::ostringstream out;
    const std::string rule_file(rules_path.string());
    const std::string_view map_args[]{rule_file};
    atperson::journal::run_journal_command(out, graph, journal_path, root, model_path, "map",
                                           map_args, 1u, 1789600000, "2026-09-17T20:00:00Z");
    const JournalContents after = atperson::load_journal(journal_path);
    assert(after.valence.empty());
    assert(graph.stats().node_count == 0u);
}

void test_map_rejects_bad_arguments() {
    const auto root = scratch_dir("args");
    LanguageGraph graph;
    std::ostringstream out;
    /* map with no rule file: bad arguments, exit 2, nothing mutated. */
    const int status = atperson::journal::run_journal_command(
        out, graph, root / "action-journal.jsonl", root, root / "model.bin", "map",
        nullptr, 0u, 0, "2026-09-17T20:00:00Z");
    assert(status == 2);
}

} // namespace

int main() {
    test_parse_rejects_malformed_tables();
    test_parse_and_match();
    test_event_window_matching();
    test_map_command_end_to_end();
    test_map_skips_unknown_tokens();
    test_map_rejects_bad_arguments();
    std::printf("journal rules tests passed\n");
    return 0;
}
