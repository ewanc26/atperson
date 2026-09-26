/* Ingestion and catch-up over the real durable pipeline: multi-run
 * traversal, crash recovery, retry, deduplication, withdrawal, planning
 * abstention, and cross-run determinism.
 *
 * Part of the deterministic end-to-end lifecycle harness; see
 * tests/support/lifecycle_harness.hpp for the shared fixtures. Offline and
 * deterministic. */

#include "support/lifecycle_harness.hpp"

#include "atperson/action.hpp"
#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace atperson::e2e {
namespace {

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
    Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 4u);
    for (const auto &entry : ledger.entries()) {
        assert(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    }

    auto state =
        atperson::load_ingestion_state(scenario.state_file, "https://bsky.social", "did:plc:abc");
    assert(!state.catchup.active);
    assert(!state.catchup.cursor);
    assert(state.checkpoint.pages_completed == 3u);
    assert(state.checkpoint.observations_seen == 4u);
    assert(state.checkpoint.generation == 3u);

    /* The snapshot carries all four observations. */
    const auto graph = LanguageGraph::load(scenario.model_file);
    assert(graph.stats().observations == 4u);
}

void test_crash_between_ledger_and_snapshot_recovers_via_replay() {
    /* Run 1 completes normally and durably learns one observation. Run 2
     * commits two more ledger entries, then the process dies before the
     * snapshot/state saves. The ledger now claims LEARNED for items the
     * snapshot never trained on — the recovery path is `rebuild` (replay
     * from the ledger), which is exactly what the CLI does next. */
    Scenario scenario("crash-recovery");
    ScriptedFeed feed({
        {{obs("at://e2e/c1", "alpha beta")}, "1"},
        {{obs("at://e2e/c2", "gamma delta"), obs("at://e2e/c3", "epsilon zeta")}, std::nullopt},
    });

    (void)scenario.run(feed, 1);

    /* Crash run: ledger commits, snapshot and state do not. */
    const auto crashed = scenario.run(feed, 1, /*crash_after_sync=*/true);
    assert(crashed.observations_seen == 2u);
    assert(crashed.learned == 2u);

    /* The ledger is authoritative: three committed entries. */
    Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 3u);

    /* The stale snapshot only saw the first observation. */
    const auto stale = LanguageGraph::load(scenario.model_file);
    assert(stale.stats().observations == 1u);

    /* Recovery: replay the ledger into a fresh graph and save. This is the
     * rebuild command's composition, byte-for-byte. */
    LanguageGraph rebuilt;
    const auto report = rebuilt.replay(ledger);
    assert(report.replayed == 3u);
    rebuilt.save(scenario.model_file);

    const auto recovered = LanguageGraph::load(scenario.model_file);
    assert(recovered.stats().observations == 3u);
}

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
    const auto failing = [](const std::optional<std::string> &) -> SyncPage {
        throw std::runtime_error("transport failure");
    };
    bool threw = false;
    try {
        (void)scenario.run(failing, 1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    auto state =
        atperson::load_ingestion_state(scenario.state_file, "https://bsky.social", "did:plc:abc");
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("1"));
    {
        Ledger ledger(scenario.ledger_file);
        assert(ledger.count() == 1u);
    }

    /* Retry with the healthy feed: resumes from "1", reaches exhaustion. */
    const auto resumed = scenario.run(healthy, 1);
    assert(resumed.exhausted);
    assert(resumed.observations_seen == 1u);
    Ledger reloaded(scenario.ledger_file);
    assert(reloaded.count() == 2u);
}

void test_duplicates_across_restarts_are_suppressed() {
    /* The feed replays the same page every traversal: a fresh traversal
     * (null cursor) always returns p1 with a duplicate of an item the
     * ledger already committed. Ledger dedup must suppress re-training
     * across restarts. */
    Scenario scenario("dedup");
    const auto feed = [](const std::optional<std::string> &cursor) -> SyncPage {
        if (!cursor) {
            return SyncPage{
                .items = {obs("at://e2e/d1", "alpha beta"), obs("at://e2e/d2", "gamma")},
                .next_cursor = std::nullopt,
            };
        }
        return SyncPage{};
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

    Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 2u);
    const auto graph = LanguageGraph::load(scenario.model_file);
    assert(graph.stats().observations == 2u);
}

void test_edits_and_deletions_withdraw_from_learned_state() {
    Scenario scenario("withdrawal");
    ScriptedFeed feed({
        {{obs("at://e2e/w1", "alpha beta"), obs("at://e2e/w2", "gamma delta"),
          obs("at://e2e/w3", "epsilon zeta", "did:plc:other")},
         std::nullopt},
    });

    (void)scenario.run(feed, 1);
    Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 3u);

    /* A deleted post: withdraw every entry from its source URI. */
    assert(ledger.withdraw_source("at://e2e/w2") == 1u);
    /* A blocked author: withdraw every entry they authored. */
    assert(ledger.withdraw_author("did:plc:other") == 1u);

    /* Learned state reflects the withdrawals only after a rebuild. */
    LanguageGraph rebuilt;
    const auto report = rebuilt.replay(ledger);
    assert(report.replayed == 1u);
    assert(report.excluded_withdrawn == 2u);
    rebuilt.save(scenario.model_file);

    const auto recovered = LanguageGraph::load(scenario.model_file);
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
    Ledger final_ledger(scenario.ledger_file);
    assert(final_ledger.count() == 3u);
}

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
    LanguageGraph empty;
    const auto empty_decision = empty.action_decide("alpha");
    assert(empty_decision.abstained);
    assert(empty_decision.abstain_reason == ATP_ACTION_ABSTAIN_NO_CANDIDATES);

    (void)scenario.run(feed, 1);
    const auto graph = LanguageGraph::load(scenario.model_file);

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

void test_equivalent_runs_produce_equivalent_final_state() {
    /* Two independent scenario runs with the same scripted feed and the
     * same run boundaries must produce identical durable learned state.
     * The ledger's content digests and the snapshot bytes carry no
     * wall-clock values, so byte equality is required. */
    const auto execute = [](const char *tag) {
        Scenario scenario(tag);
        ScriptedFeed feed({
            {{obs("at://e2e/x1", "alpha beta"),
              obs("at://e2e/x2", "beta gamma", "did:plc:other", "2026-09-15T12:00:00Z")},
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

} // namespace

void run_ingestion_scenarios() {
    test_multi_run_catchup_with_restarts();
    test_crash_between_ledger_and_snapshot_recovers_via_replay();
    test_transient_failure_is_retryable();
    test_duplicates_across_restarts_are_suppressed();
    test_edits_and_deletions_withdraw_from_learned_state();
    test_planning_abstention_on_recovered_state();
    test_equivalent_runs_produce_equivalent_final_state();
}

} // namespace atperson::e2e
