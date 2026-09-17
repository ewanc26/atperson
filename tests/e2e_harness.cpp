/* Deterministic end-to-end lifecycle harness (issue #28).
 *
 * Exercises the real durable pipeline — ledger files, graph snapshots,
 * ingestion state — across multi-run lifecycles without live network access.
 * The only fixture is the page fetcher: a scripted feed that can inject
 * pagination, transient failures, duplicates, edits and deletions.
 *
 * Scenarios run, stop, restart from disk, continue, and assert the final
 * ledger/snapshot/state. Equivalent scenario runs must produce equivalent
 * final learned state (cross-run determinism). All timestamps are fixed so
 * no wall-clock value can leak into learned state. */

#include "atperson/action.hpp"
#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "ingestion_state.hpp"
#include "sync_engine.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/* ---------------------------------------------------------------- */
/* Scratch directories                                               */
/* ---------------------------------------------------------------- */

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-e2e-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/* ---------------------------------------------------------------- */
/* Scripted feed fixture                                             */
/* ---------------------------------------------------------------- */

/* One scripted page: items plus the next cursor. `failure` makes the
 * fetcher throw instead of returning the page (retryable transport
 * failure). */
struct ScriptedPage {
    std::vector<atperson::SyncObservation> items;
    std::optional<std::string> next_cursor;
    bool failure{false};
};

atperson::SyncObservation obs(const std::string &uri, const std::string &text,
                              const std::string &author = "did:plc:author",
                              const std::string &created_at = "2026-09-16T00:00:00Z") {
    return atperson::SyncObservation{
        .text = text,
        .source_uri = uri,
        .author_did = author,
        .created_at = created_at,
    };
}

/* A feed is a function from the incoming cursor to the scripted page. The
 * default feed is a fixed page list: null cursor -> page 0 -> page 1 ...
 * until a scripted page has no next_cursor (exhausted). */
class ScriptedFeed {
  public:
    explicit ScriptedFeed(std::vector<ScriptedPage> pages) : pages_(std::move(pages)) {}

    atperson::SyncPage operator()(const std::optional<std::string> &cursor) {
        const std::size_t index = !cursor ? 0u : static_cast<std::size_t>(std::stoul(*cursor));
        if (index >= pages_.size()) {
            return atperson::SyncPage{}; /* exhausted */
        }
        const ScriptedPage &page = pages_[index];
        if (page.failure) {
            throw std::runtime_error("scripted transport failure");
        }
        return atperson::SyncPage{.items = page.items, .next_cursor = page.next_cursor};
    }

  private:
    std::vector<ScriptedPage> pages_;
};

/* ---------------------------------------------------------------- */
/* Durable scenario runner                                           */
/* ---------------------------------------------------------------- */

/* Paths and run() mirror the CLI command composition (sync.cpp run order):
 * load state -> run_sync -> save snapshot -> save state. The runner is the
 * fixture equivalent of `atperson sync N` with the scripted feed standing in
 * for the network client. */
struct Scenario {
    std::filesystem::path dir;
    std::filesystem::path ledger_file;
    std::filesystem::path model_file;
    std::filesystem::path state_file;

    explicit Scenario(const char *tag)
        : dir(scratch_dir(tag)), ledger_file(dir / "ledger.bin"), model_file(dir / "graph.snap"),
          state_file(dir / "state.json") {}

    /* One process run: open the durable files, run the sync, persist.
     * `crash_after` simulates an unclean exit after the given run_sync
     * return but before the snapshot/state saves (crash point between the
     * ledger commits and the snapshot write). */
    atperson::SyncResult run(const atperson::SyncPageFetcher &feed, int max_pages,
                             bool crash_after_sync = false) {
        atperson::LanguageGraph graph =
            std::filesystem::exists(model_file) ? atperson::LanguageGraph::load(model_file)
                                                : atperson::LanguageGraph();
        atperson::Ledger ledger(ledger_file);
        auto state = atperson::load_ingestion_state(state_file, "https://bsky.social",
                                                    "did:plc:abc");

        atperson::SyncLimits limits;
        limits.max_pages = max_pages;
        const auto result = atperson::run_sync(graph, ledger, state, feed, limits);
        if (crash_after_sync) {
            return result; /* snapshot and state never saved */
        }
        graph.save(model_file);
        state.checkpoint.generation++;
        atperson::save_ingestion_state(state, state_file);
        return result;
    }
};

/* A canonical digest of the final learned state: snapshot bytes plus the
 * durable ledger contents. Equivalent scenario runs must produce equal
 * digests. */
std::string final_state_digest(const Scenario &scenario) {
    std::ostringstream out;
    if (std::filesystem::exists(scenario.model_file)) {
        std::ifstream input(scenario.model_file, std::ios::binary);
        out << input.rdbuf();
    }
    atperson::Ledger ledger(scenario.ledger_file);
    for (const auto &entry : ledger.entries()) {
        out << entry.id << '|' << entry.source_id << '|' << entry.author_did << '|'
            << entry.observed_at << '|' << entry.content_digest << '|' << entry.outcome << '\n';
    }
    return out.str();
}

/* ---------------------------------------------------------------- */
/* Scenario: multi-run catch-up with restarts                        */
/* ---------------------------------------------------------------- */

void test_multi_run_catchup_with_restarts() {
    /* Three scripted pages; the process only completes one page per run
     * (max_pages=1), so the traversal spans three runs with two restarts
     * from disk. */
    Scenario scenario("multi-run");
    ScriptedFeed feed({
        {{obs("at://e2e/p1", "alpha beta"), obs("at://e2e/p2", "beta gamma")}, "1"},
        {{obs("at://e2e/p3", "gamma delta")}, "2"},
        {{obs("at://e2e/p4", "delta epsilon")}, std::nullopt},
    });

    const auto first = scenario.run(feed, 1);
    assert(first.pages_completed == 1u);
    assert(!first.exhausted);

    /* Restart 1: cursor resumes from page 1. */
    const auto second = scenario.run(feed, 1);
    assert(second.pages_completed == 1u);
    assert(!second.exhausted);

    /* Restart 2: final page exhausts the timeline. */
    const auto third = scenario.run(feed, 1);
    assert(third.exhausted);
    assert(third.observations_seen == 1u);

    /* Final durable state: four ledgered entries, all learned, cursor
     * cleared, checkpoint counters accumulated across runs. */
    atperson::Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 4u);
    for (const auto &entry : ledger.entries()) {
        assert(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    }

    auto state = atperson::load_ingestion_state(scenario.state_file, "https://bsky.social",
                                               "did:plc:abc");
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.checkpoint.pages_completed == 3u);
    assert(state.checkpoint.observations_seen == 4u);
    assert(state.checkpoint.generation == 3u);

    /* The snapshot carries all four observations. */
    const auto graph = atperson::LanguageGraph::load(scenario.model_file);
    assert(graph.stats().observations == 4u);
}

/* ---------------------------------------------------------------- */
/* Scenario: crash between ledger commits and snapshot save          */
/* ---------------------------------------------------------------- */

void test_crash_between_ledger_and_snapshot_recovers_via_replay() {
    /* Run 1 completes normally and durably learns one observation. Run 2
     * commits two more ledger entries, then the process dies before the
     * snapshot/state saves. The ledger now claims LEARNED for items the
     * snapshot never trained on — the recovery path is `rebuild` (replay
     * from the ledger), which is exactly what the CLI does next. */
    Scenario scenario("crash-recovery");
    ScriptedFeed feed({
        {{obs("at://e2e/c1", "alpha beta")}, "1"},
        {{obs("at://e2e/c2", "gamma delta"), obs("at://e2e/c3", "epsilon zeta")},
         std::nullopt},
    });

    (void)scenario.run(feed, 1);

    /* Crash run: ledger commits, snapshot and state do not. */
    const auto crashed = scenario.run(feed, 1, /*crash_after_sync=*/true);
    assert(crashed.observations_seen == 2u);
    assert(crashed.learned == 2u);

    /* The ledger is authoritative: three committed entries. */
    atperson::Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 3u);

    /* The stale snapshot only saw the first observation. */
    const auto stale = atperson::LanguageGraph::load(scenario.model_file);
    assert(stale.stats().observations == 1u);

    /* Recovery: replay the ledger into a fresh graph and save. This is the
     * rebuild command's composition, byte-for-byte. */
    atperson::LanguageGraph rebuilt;
    const auto report = rebuilt.replay(ledger);
    assert(report.replayed == 3u);
    rebuilt.save(scenario.model_file);

    const auto recovered = atperson::LanguageGraph::load(scenario.model_file);
    assert(recovered.stats().observations == 3u);
}

/* ---------------------------------------------------------------- */
/* Scenario: transient failure then retryable continuation           */
/* ---------------------------------------------------------------- */

void test_transient_failure_is_retryable() {
    Scenario scenario("transient");
    ScriptedFeed healthy({
        {{obs("at://e2e/t1", "alpha beta")}, "1"},
        {{obs("at://e2e/t2", "gamma delta")}, std::nullopt},
    });

    /* First run: page 0 commits, cursor checkpoints at "1". */
    (void)scenario.run(healthy, 1);

    /* Second run: the transport fails on the page after the cursor. The
     * run throws; the persisted cursor and snapshot must be untouched. */
    const auto failing = [](const std::optional<std::string> &) -> atperson::SyncPage {
        throw std::runtime_error("transport failure");
    };
    bool threw = false;
    try {
        (void)scenario.run(failing, 1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    auto state = atperson::load_ingestion_state(scenario.state_file, "https://bsky.social",
                                               "did:plc:abc");
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("1"));
    {
        atperson::Ledger ledger(scenario.ledger_file);
        assert(ledger.count() == 1u);
    }

    /* Retry with the healthy feed: resumes from "1", reaches exhaustion. */
    const auto resumed = scenario.run(healthy, 1);
    assert(resumed.exhausted);
    assert(resumed.observations_seen == 1u);
    atperson::Ledger reloaded(scenario.ledger_file);
    assert(reloaded.count() == 2u);
}

/* ---------------------------------------------------------------- */
/* Scenario: duplicate observations across restarts                  */
/* ---------------------------------------------------------------- */

void test_duplicates_across_restarts_are_suppressed() {
    /* The feed replays the same page every traversal: a fresh traversal
     * (null cursor) always returns p1 with a duplicate of an item the
     * ledger already committed. Ledger dedup must suppress re-training
     * across restarts. */
    Scenario scenario("dedup");
    const auto feed = [](const std::optional<std::string> &cursor) -> atperson::SyncPage {
        if (!cursor) {
            return atperson::SyncPage{
                .items = {obs("at://e2e/d1", "alpha beta"), obs("at://e2e/d2", "gamma")},
                .next_cursor = std::nullopt,
            };
        }
        return atperson::SyncPage{};
    };

    const auto first = scenario.run(feed, 1);
    assert(first.learned == 2u);
    assert(first.duplicates == 0u);

    /* A second full traversal of the same timeline: both items are
     * duplicates; the ledger count and snapshot stay unchanged. */
    const auto second = scenario.run(feed, 1);
    assert(second.observations_seen == 2u);
    assert(second.duplicates == 2u);
    assert(second.learned == 0u);

    atperson::Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 2u);
    const auto graph = atperson::LanguageGraph::load(scenario.model_file);
    assert(graph.stats().observations == 2u);
}

/* ---------------------------------------------------------------- */
/* Scenario: edits and deletions (withdrawal flows)                 */
/* ---------------------------------------------------------------- */

void test_edits_and_deletions_withdraw_from_learned_state() {
    Scenario scenario("withdrawal");
    ScriptedFeed feed({
        {{obs("at://e2e/w1", "alpha beta"), obs("at://e2e/w2", "gamma delta"),
          obs("at://e2e/w3", "epsilon zeta", "did:plc:other")},
         std::nullopt},
    });

    (void)scenario.run(feed, 1);
    atperson::Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 3u);

    /* A deleted post: withdraw every entry from its source URI. */
    assert(ledger.withdraw_source("at://e2e/w2") == 1u);
    /* A blocked author: withdraw every entry they authored. */
    assert(ledger.withdraw_author("did:plc:other") == 1u);

    /* Learned state reflects the withdrawals only after a rebuild. */
    atperson::LanguageGraph rebuilt;
    const auto report = rebuilt.replay(ledger);
    assert(report.replayed == 1u);
    assert(report.excluded_withdrawn == 2u);
    rebuilt.save(scenario.model_file);

    const auto recovered = atperson::LanguageGraph::load(scenario.model_file);
    assert(recovered.stats().observations == 1u);

    /* A re-run of the same feed: withdrawn observations stay withdrawn.
     * WITHDRAWN is a committed outcome, so the (source, digest) dedup key
     * survives the withdrawal — a deleted or excluded post reappearing in
     * the feed is a duplicate, never re-learned. Withdrawal is permanent
     * exclusion. */
    const auto rerun = scenario.run(feed, 1);
    assert(rerun.observations_seen == 3u);
    assert(rerun.duplicates == 3u);
    assert(rerun.learned == 0u);
    atperson::Ledger final_ledger(scenario.ledger_file);
    assert(final_ledger.count() == 3u);
}

/* ---------------------------------------------------------------- */
/* Scenario: planning abstention on the recovered graph              */
/* ---------------------------------------------------------------- */

void test_planning_abstention_on_recovered_state() {
    /* A graph that has learned nothing must abstain from planning with
     * the documented reason, and a graph that has learned must produce a
     * deterministic decision. This pins the planning boundary into the
     * lifecycle: recovered state drives decisions, not mocks. */
    Scenario scenario("abstention");
    ScriptedFeed feed({
        {{obs("at://e2e/a1", "alpha beta alpha beta")}, std::nullopt},
    });

    /* Empty graph: no candidates. */
    atperson::LanguageGraph empty;
    const auto empty_decision = empty.action_decide("alpha");
    assert(empty_decision.abstained);
    assert(empty_decision.abstain_reason == ATP_ACTION_ABSTAIN_NO_CANDIDATES);

    (void)scenario.run(feed, 1);
    const auto graph = atperson::LanguageGraph::load(scenario.model_file);

    /* Recovered graph: the learned context produces a non-abstaining,
     * deterministic decision. */
    const auto first = graph.action_decide("alpha");
    const auto second = graph.action_decide("alpha");
    assert(!first.abstained);
    assert(first.abstain_reason == ATP_ACTION_ABSTAIN_NONE);
    assert(first.plan.step_count == second.plan.step_count);
    assert(first.plan.score == second.plan.score);

    /* Unknown context on a trained graph still abstains. */
    const auto unknown = graph.action_decide("zzz qqq");
    assert(unknown.abstained);
    assert(unknown.abstain_reason == ATP_ACTION_ABSTAIN_NO_CANDIDATES);
}

/* ---------------------------------------------------------------- */
/* Scenario: cross-run determinism                                   */
/* ---------------------------------------------------------------- */

void test_equivalent_runs_produce_equivalent_final_state() {
    /* Two independent scenario runs with the same scripted feed and the
     * same run boundaries must produce identical durable learned state.
     * The ledger's content digests and the snapshot bytes carry no
     * wall-clock values, so byte equality is required. */
    const auto execute = [](const char *tag) {
        Scenario scenario(tag);
        ScriptedFeed feed({
            {{obs("at://e2e/x1", "alpha beta"), obs("at://e2e/x2", "beta gamma",
                                                     "did:plc:other", "2026-09-15T12:00:00Z")},
             "1"},
            {{obs("at://e2e/x3", "gamma delta")}, "2"},
            {{obs("at://e2e/x4", "delta epsilon")}, std::nullopt},
        });
        (void)scenario.run(feed, 1);
        (void)scenario.run(feed, 1);
        (void)scenario.run(feed, 2);
        return final_state_digest(scenario);
    };

    const auto one = execute("determinism-a");
    const auto two = execute("determinism-b");
    assert(one == two);
}

/* ---------------------------------------------------------------- */

} // namespace

int main() {
    test_multi_run_catchup_with_restarts();
    test_crash_between_ledger_and_snapshot_recovers_via_replay();
    test_transient_failure_is_retryable();
    test_duplicates_across_restarts_are_suppressed();
    test_edits_and_deletions_withdraw_from_learned_state();
    test_planning_abstention_on_recovered_state();
    test_equivalent_runs_produce_equivalent_final_state();

    std::printf("e2e harness passed\n");
    return 0;
}
