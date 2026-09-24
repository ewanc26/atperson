/* Experience-derived drives (#148): curiosity (novelty x adjacency) and
 * reciprocity (referenced-outbound-action signals) as bounded, inspectable
 * scheduler-initiation signals. Deterministic and offline over a real graph
 * and journal; no network. */
#include "scheduler/drives.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "journal/store.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::JournalEvent;
using atperson::LanguageGraph;
using atperson::Ledger;
using atperson::drives::ContextCandidate;
using atperson::drives::Signals;
using atperson::drives::compute_drive_signals;
using atperson::drives::order_candidates;
using atperson::drives::select_candidates;

constexpr std::int64_t NOW = 1'700'000'000;
constexpr std::int64_t kDay = 24 * 3600;
constexpr std::int64_t kWeeks = 7 * kDay;

/* A graph where "alpha beta" was observed once (familiarity exactly 1.0) and
 * "sigma tau" was observed repeatedly (familiarity ~7.46, nearly bored). Two
 * episodes are remembered from "did:plc:other" with episode text that does
 * not touch the alpha/beta or sigma/tau tokens, so the encounter count is
 * the authoritative learned evidence for the author curiosity component. */
LanguageGraph curiosity_graph() {
    LanguageGraph graph;
    graph.observe("alpha beta", "at://drives/observe/1");
    for (std::size_t i = 0u; i < 8u; ++i) {
        graph.observe("sigma tau", "at://drives/saturate/" + std::to_string(i));
    }
    const std::uint64_t digest = Ledger::digest("alpha beta");
    graph.remember("episode marker", "at://drives/remember/1", "did:plc:other", NOW, digest, 1u,
                   1001u);
    graph.remember("episode marker", "at://drives/remember/2", "did:plc:other", NOW, digest, 1u,
                   1002u);
    return graph;
}

/* A journal with one executed action and one reply event referencing it. */
JournalContents journal_with_reply(std::int64_t event_epoch) {
    JournalContents journal;
    JournalAction action;
    action.id = "some-rkey";
    action.kind = "post";
    action.text = "hello";
    action.digest = "abcdef0123456789";
    action.outcome = JournalActionOutcome::Executed;
    action.uri = "at://did:plc:self/app.bsky.feed.post/abc";
    action.at = "2023-11-14T22:13:20Z";
    journal.actions.push_back(action);

    JournalEvent event;
    event.action_id = "some-rkey";
    event.event_uri = "at://did:plc:fan/app.bsky.feed.post/xyz";
    event.author_did = "did:plc:fan";
    event.via = "reply";
    const auto as_rfc3339 = [](std::int64_t epoch) {
        /* Deterministic UTC formatting; only the Z-suffix + rough digits are
         * needed for window comparison, which parses the full timestamp. */
        const std::time_t value = static_cast<std::time_t>(epoch);
        std::tm utc{};
        gmtime_r(&value, &utc);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                      utc.tm_min, utc.tm_sec);
        return std::string(buffer);
    };
    event.at = as_rfc3339(event_epoch);
    journal.events.push_back(event);
    return journal;
}

ContextCandidate candidate_at(std::uint64_t id, std::string payload, std::string author) {
    ContextCandidate candidate;
    candidate.ledger_id = id;
    candidate.payload = std::move(payload);
    candidate.source_id = "at://did:plc:any/app.bsky.feed.post/" + std::to_string(id);
    candidate.author_did = std::move(author);
    candidate.observed_at = static_cast<std::uint64_t>(NOW);
    return candidate;
}

void test_fresh_state_has_zero_drives() {
    const LanguageGraph graph; /* empty: nothing observed, no episodes */
    const JournalContents journal; /* empty: nothing executed, no events */
    const std::vector<ContextCandidate> candidates{candidate_at(1u, "alpha beta", "")};

    const std::vector<Signals> signals =
        compute_drive_signals(graph, candidates, journal, NOW);
    assert(signals.size() == 1u);
    assert(signals[0].curiosity == 0.0f);
    assert(signals[0].reciprocity == 0.0f);
    assert(std::string_view(signals[0].reciprocity_source) ==
           atperson::drives::kReciprocityNone);
}

void test_curiosity_ranks_partial_novelty() {
    const LanguageGraph graph = curiosity_graph();
    const JournalContents journal;
    /* Partially known topic: "alpha" familiar (novelty 1/(1+1)), the rest
     * unknown: curiosity = 0.5 * 1/3, strictly between zero and one. */
    const ContextCandidate partial = candidate_at(1u, "alpha gamma delta", "did:plc:unseen");
    /* Repeatedly exposed topic: novelty decayed toward zero. */
    const ContextCandidate saturated = candidate_at(2u, "sigma tau", "did:plc:unseen");
    /* Entirely unseen topic: no adjacency at all. */
    const ContextCandidate fresh = candidate_at(3u, "delta kappa", "did:plc:unseen");

    const std::vector<Signals> signals =
        compute_drive_signals(graph, {partial, saturated, fresh}, journal, NOW);
    assert(signals[0].curiosity > 0.05f && signals[0].curiosity < 0.9f);
    assert(signals[1].curiosity > 0.0f);
    assert(signals[1].curiosity < signals[0].curiosity);
    assert(signals[2].curiosity == 0.0f);
    assert(signals[0].reciprocity == 0.0f);
}

void test_curiosity_author_encounters() {
    const LanguageGraph graph = curiosity_graph();
    const JournalContents journal;
    /* "did:plc:other" has two remembered episodes: novelty 1/(1+2). The
     * topic ("sigma tau") is nearly bored out, so the author component
     * dominates the otherwise smaller topic signal. */
    const ContextCandidate seen = candidate_at(1u, "sigma tau", "did:plc:other");
    /* A never-seen author contributes nothing on its own. */
    const ContextCandidate unseen = candidate_at(2u, "gamma", "did:plc:stranger");

    const std::vector<Signals> signals = compute_drive_signals(graph, {seen, unseen}, journal, NOW);
    const float expected = 1.0f / 3.0f;
    assert(std::fabs(signals[0].curiosity - expected) < 0.01f);
    assert(signals[1].curiosity == 0.0f);
}

void test_reciprocity_prefers_the_referencing_record() {
    const LanguageGraph graph = curiosity_graph();
    const JournalContents journal = journal_with_reply(NOW - kDay);

    /* The exact public record that referenced an executed action. */
    ContextCandidate exact = candidate_at(1u, "alpha beta", "did:plc:fan");
    exact.source_id = "at://did:plc:fan/app.bsky.feed.post/xyz";
    /* Same author, different record, inside the window: weaker signal. */
    const ContextCandidate author = candidate_at(2u, "alpha beta", "did:plc:fan");
    /* Unrelated author and record: no reciprocity. */
    const ContextCandidate other = candidate_at(3u, "alpha beta", "did:plc:stranger");

    const std::vector<Signals> signals =
        compute_drive_signals(graph, {author, exact, other}, journal, NOW);
    assert(signals[0].reciprocity == 0.5f);
    assert(std::string_view(signals[0].reciprocity_source) ==
           atperson::drives::kReciprocityAuthor);
    assert(signals[1].reciprocity == 1.0f);
    assert(std::string_view(signals[1].reciprocity_source) ==
           atperson::drives::kReciprocityEvent);
    assert(signals[2].reciprocity == 0.0f);

    /* Ordering: the referencing record first, then its author, then the
     * unrelated context. */
    const std::vector<std::size_t> order = order_candidates(signals);
    assert(order == (std::vector<std::size_t>{1u, 0u, 2u}));
}

void test_reciprocity_author_window_expires() {
    const LanguageGraph graph = curiosity_graph();
    /* The referencing event happened more than a week ago. */
    const JournalContents journal = journal_with_reply(NOW - 8 * kDay);

    const ContextCandidate author = candidate_at(1u, "alpha beta", "did:plc:fan");
    const std::vector<Signals> signals = compute_drive_signals(graph, {author}, journal, NOW);
    assert(signals[0].reciprocity == 0.0f);
    assert(std::string_view(signals[0].reciprocity_source) ==
           atperson::drives::kReciprocityNone);
}

void test_order_is_deterministic_and_stable() {
    const LanguageGraph graph;
    const JournalContents journal;
    const std::vector<ContextCandidate> candidates{
        candidate_at(1u, "alpha", "did:plc:a"),
        candidate_at(2u, "beta", "did:plc:b"),
        candidate_at(3u, "gamma", "did:plc:c")};
    const std::vector<Signals> signals = compute_drive_signals(graph, candidates, journal, NOW);

    /* All-zero signals: the stable sort preserves original order. */
    const std::vector<std::size_t> first = order_candidates(signals);
    assert(first == (std::vector<std::size_t>{0u, 1u, 2u}));
    assert(order_candidates(signals) == first);
    assert(kWeeks == atperson::drives::kReciprocityWindowSeconds);
}

void test_select_candidates_mirrors_scheduler_selection() {
    const auto root =
        std::filesystem::temp_directory_path() / "atperson-drives-select";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        Ledger ledger(root / "ledger.bin");
        std::uint64_t id = 0u;
        /* oldest committed, then a skipped PENDING, then newest committed. */
        assert(ledger.append("at://drives/ledger/1", "did:plc:a", NOW - 60, Ledger::digest("alpha"),
                             2u, ATP_LEDGER_OUTCOME_LEARNED, "alpha", &id) ==
               atperson::LedgerResult::New);
        assert(ledger.append("at://drives/ledger/2", "did:plc:b", NOW - 30, Ledger::digest("beta"),
                             2u, ATP_LEDGER_OUTCOME_PENDING, "beta", &id) ==
               atperson::LedgerResult::New);
        assert(ledger.append("at://drives/ledger/3", "did:plc:c", NOW, Ledger::digest("gamma"), 2u,
                             ATP_LEDGER_OUTCOME_LEARNED, "gamma", &id) ==
               atperson::LedgerResult::New);
        /* withdrawn: not durable authority */
        assert(ledger.append("at://drives/ledger/4", "did:plc:d", NOW + 30, Ledger::digest("delta"),
                             2u, ATP_LEDGER_OUTCOME_WITHDRAWN, "delta", &id) ==
               atperson::LedgerResult::New);

        const std::vector<ContextCandidate> selected = select_candidates(ledger, 8u);
        /* Newest committed first (id 3), the PENDING and WITHDRAWN entries
         * are skipped, the oldest committed last. */
        assert(selected.size() == 2u);
        assert(selected[0].ledger_id == 3u);
        assert(selected[0].payload == "gamma");
        assert(selected[1].ledger_id == 1u);
        assert(selected[1].payload == "alpha");
        assert(selected[0].author_did == "did:plc:c");

        /* Bounded by max_contexts. */
        const std::vector<ContextCandidate> bounded = select_candidates(ledger, 1u);
        assert(bounded.size() == 1u);
        assert(bounded[0].ledger_id == 3u);
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_fresh_state_has_zero_drives();
    test_curiosity_ranks_partial_novelty();
    test_curiosity_author_encounters();
    test_reciprocity_prefers_the_referencing_record();
    test_reciprocity_author_window_expires();
    test_order_is_deterministic_and_stable();
    test_select_candidates_mirrors_scheduler_selection();
    std::cout << "atperson-drives: all assertions passed\n";
    return 0;
}