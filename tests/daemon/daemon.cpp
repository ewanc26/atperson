/* Long-running ingestion daemon (#21): scheduling, bounded work, retry
 * backoff, snapshot cadence, graceful shutdown and restart resume. Everything
 * runs offline: page fetchers, sleeps, stopping and persistence are injected
 * into run_daemon, so no live network or real waiting is involved. */
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "daemon/backoff.hpp"
#include "daemon/config.hpp"
#include "daemon/failure.hpp"
#include "daemon/loop.hpp"
#include "engine.hpp"
#include "ingestion/state.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using atperson::DaemonConfig;
using atperson::DaemonHooks;
using atperson::DaemonPersistence;
using atperson::DaemonRunReport;
using atperson::SyncLimits;
using atperson::SyncPage;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-daemon-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

atperson::SyncObservation obs(const std::string &uri, const std::string &text) {
    return atperson::SyncObservation{
        .text = text,
        .source_uri = uri,
        .author_did = "did:plc:author",
        .created_at = "2026-09-16T00:00:00Z",
    };
}

/* Two-page fixture: head has p1/p2 and cursor c1; c1 has p3 and is drained. */
SyncPage two_page_feed(const std::optional<std::string> &cursor) {
    if (!cursor) {
        return SyncPage{
            .items = {obs("at://fixture/p1", "alpha beta"), obs("at://fixture/p2", "gamma")},
            .next_cursor = std::string("c1"),
        };
    }
    return SyncPage{
        .items = {obs("at://fixture/p3", "delta")},
        .next_cursor = std::nullopt,
    };
}

/* Always the same drained head page: every later cycle sees duplicates. */
SyncPage drained_feed(const std::optional<std::string> &) {
    return SyncPage{
        .items = {obs("at://fixture/p1", "alpha beta"), obs("at://fixture/p2", "gamma")},
        .next_cursor = std::nullopt,
    };
}

DaemonConfig no_jitter_config() {
    DaemonConfig config;
    config.backoff.initial = std::chrono::milliseconds(100);
    config.backoff.maximum = std::chrono::milliseconds(1000);
    config.backoff.factor = 2.0;
    config.backoff.jitter = 0.0;
    return config;
}

SyncLimits default_limits() {
    SyncLimits limits;
    limits.page_size = 50;
    limits.max_pages = 1;
    limits.max_observations = 0;
    return limits;
}

/* ---------------------------------------------------------------- */
/* Backoff                                                           */
/* ---------------------------------------------------------------- */

void test_backoff_grows_deterministically_and_is_bounded() {
    const auto config = no_jitter_config().backoff;
    atperson::Backoff backoff(config);
    assert(backoff.next() == std::chrono::milliseconds(100));
    assert(backoff.next() == std::chrono::milliseconds(200));
    assert(backoff.next() == std::chrono::milliseconds(400));
    assert(backoff.next() == std::chrono::milliseconds(800));
    assert(backoff.next() == std::chrono::milliseconds(1000));
    assert(backoff.next() == std::chrono::milliseconds(1000));
    assert(backoff.consecutive_failures() == 6u);

    backoff.reset();
    assert(backoff.consecutive_failures() == 0u);
    assert(backoff.next() == std::chrono::milliseconds(100));
}

void test_backoff_jitter_is_deterministic_and_bounded() {
    auto config = no_jitter_config().backoff;
    config.jitter = 0.5;

    atperson::Backoff first(config, 42u);
    atperson::Backoff second(config, 42u);

    std::chrono::milliseconds previous{};
    for (int i = 0; i < 8; ++i) {
        const auto a = first.next();
        const auto b = second.next();
        assert(a == b);        /* same seed => identical sequence */
        assert(a >= previous); /* non-decreasing */
        assert(a <= config.maximum);
        previous = a;
    }

    /* A different seed still stays within the bound. */
    atperson::Backoff other(config, 7u);
    for (int i = 0; i < 8; ++i) {
        assert(other.next() <= config.maximum);
    }
}

/* ---------------------------------------------------------------- */
/* Failure classification and config                                 */
/* ---------------------------------------------------------------- */

void test_failure_classification() {
    const atperson::RetryableError retryable("temporary");
    const std::runtime_error fatal("fatal");
    assert(atperson::is_retryable(retryable));
    assert(!atperson::is_retryable(fatal));
}

void test_config_defaults_are_valid() {
    DaemonConfig config;
    atperson::validate_daemon_config(config);
    assert(config.pages_per_cycle == 1);
    assert(config.max_cycles == 0u);
    assert(config.snapshot_every_cycles == 1u);
}

void test_config_rejects_impossible_values() {
    {
        DaemonConfig config;
        config.pages_per_cycle = 0;
        bool threw = false;
        try {
            atperson::validate_daemon_config(config);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
    {
        DaemonConfig config;
        config.snapshot_every_cycles = 0u;
        bool threw = false;
        try {
            atperson::validate_daemon_config(config);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
    {
        DaemonConfig config;
        config.backoff.maximum = std::chrono::milliseconds(10);
        config.backoff.initial = std::chrono::milliseconds(100);
        bool threw = false;
        try {
            atperson::validate_daemon_config(config);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
    {
        DaemonConfig config;
        config.backoff.jitter = 1.5;
        bool threw = false;
        try {
            atperson::validate_daemon_config(config);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }
}

void test_config_environment_overrides() {
    ::setenv("ATPERSON_DAEMON_PAGES_PER_CYCLE", "4", 1);
    ::setenv("ATPERSON_DAEMON_MAX_CYCLES", "7", 1);
    ::setenv("ATPERSON_DAEMON_POLL_MS", "2500", 1);
    ::setenv("ATPERSON_DAEMON_SNAPSHOT_EVERY", "3", 1);
    ::setenv("ATPERSON_DAEMON_BACKOFF_FACTOR", "3", 1);
    ::setenv("ATPERSON_DAEMON_BACKOFF_JITTER", "0", 1);

    const auto config = atperson::daemon_config_from_environment();
    assert(config.pages_per_cycle == 4);
    assert(config.max_cycles == 7u);
    assert(config.poll_interval == std::chrono::milliseconds(2500));
    assert(config.snapshot_every_cycles == 3u);
    assert(config.backoff.factor == 3.0);
    assert(config.backoff.jitter == 0.0);

    ::unsetenv("ATPERSON_DAEMON_PAGES_PER_CYCLE");
    ::unsetenv("ATPERSON_DAEMON_MAX_CYCLES");
    ::unsetenv("ATPERSON_DAEMON_POLL_MS");
    ::unsetenv("ATPERSON_DAEMON_SNAPSHOT_EVERY");
    ::unsetenv("ATPERSON_DAEMON_BACKOFF_FACTOR");
    ::unsetenv("ATPERSON_DAEMON_BACKOFF_JITTER");

    ::setenv("ATPERSON_DAEMON_PAGES_PER_CYCLE", "0", 1);
    bool threw = false;
    try {
        static_cast<void>(atperson::daemon_config_from_environment());
    } catch (const std::runtime_error &) {
        threw = true;
    }
    ::unsetenv("ATPERSON_DAEMON_PAGES_PER_CYCLE");
    assert(threw);
}

/* ---------------------------------------------------------------- */
/* Loop control flow                                                 */
/* ---------------------------------------------------------------- */

void test_repeated_cycles_do_not_relearn() {
    const auto dir = scratch_dir("dedup");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 3;

    DaemonPersistence persistence;
    DaemonHooks hooks;

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                             drained_feed, persistence, hooks);

    assert(report.cycles == 3u);
    assert(report.bounded);
    assert(!report.stopped);
    assert(report.learned == 2u);    /* only the first cycle trains */
    assert(report.duplicates == 4u); /* cycles two and three see duplicates */
    assert(report.observations_seen == 6u);
    assert(ledger.count() == 2u);
}

void test_transient_failure_backs_off_then_recovers() {
    const auto dir = scratch_dir("transient");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 1;

    bool failed_once = false;
    const auto feed = [&failed_once](const std::optional<std::string> &) -> SyncPage {
        if (!failed_once) {
            failed_once = true;
            throw atperson::RetryableError("service unavailable");
        }
        return drained_feed(std::nullopt);
    };

    std::vector<std::chrono::milliseconds> delays;
    DaemonHooks hooks;
    hooks.on_backoff = [&delays](std::chrono::milliseconds delay,
                                 const atperson::RetryableError &) { delays.push_back(delay); };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(), feed,
                                             DaemonPersistence{}, hooks);

    assert(report.transient_failures == 1u);
    assert(report.cycles == 1u);
    assert(report.learned == 2u);
    assert(delays.size() == 1u);
    assert(delays.front() == std::chrono::milliseconds(100));
}

void test_fatal_error_propagates_and_stops() {
    const auto dir = scratch_dir("fatal");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 5;

    const auto feed = [](const std::optional<std::string> &) -> SyncPage {
        throw std::runtime_error("durable write failed");
    };

    unsigned ingestion_saves = 0;
    DaemonPersistence persistence;
    persistence.save_ingestion = [&ingestion_saves]() { ++ingestion_saves; };

    bool threw = false;
    try {
        static_cast<void>(atperson::run_daemon(config, graph, ledger, state, default_limits(), feed,
                                               persistence, DaemonHooks{}));
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);
    /* A fatal error must not half-apply durable state or attempt a flush. */
    assert(ingestion_saves == 0u);
}

void test_graceful_stop_flushes_dirty_state() {
    const auto dir = scratch_dir("graceful");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();

    bool stop = false;
    unsigned ingestion_saves = 0;
    unsigned graph_saves = 0;

    DaemonHooks hooks;
    hooks.stop_requested = [&stop]() { return stop; };
    hooks.on_cycle = [&stop](const atperson::SyncResult &) { stop = true; };

    DaemonPersistence persistence;
    persistence.save_ingestion = [&ingestion_saves]() { ++ingestion_saves; };
    persistence.save_graph = [&graph_saves]() { ++graph_saves; };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                             drained_feed, persistence, hooks);

    assert(report.stopped);
    assert(report.cycles == 1u);
    assert(report.learned == 2u);
    assert(ingestion_saves == 2u); /* per-cycle save + graceful flush */
    assert(graph_saves == 1u);     /* dirty snapshot, no duplicate flush */
}

void test_stop_before_first_cycle_still_flushes_ingestion() {
    const auto dir = scratch_dir("stop-first");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();

    unsigned ingestion_saves = 0;
    unsigned graph_saves = 0;
    DaemonPersistence persistence;
    persistence.save_ingestion = [&ingestion_saves]() { ++ingestion_saves; };
    persistence.save_graph = [&graph_saves]() { ++graph_saves; };

    DaemonHooks hooks;
    hooks.stop_requested = []() { return true; };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                             drained_feed, persistence, hooks);

    assert(report.stopped);
    assert(report.cycles == 0u);
    assert(ingestion_saves == 1u);
    assert(graph_saves == 0u); /* nothing dirty */
}

void test_paused_gate_defers_cycles() {
    const auto dir = scratch_dir("paused");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 1;

    unsigned gate_calls = 0;
    unsigned sleep_calls = 0;
    DaemonHooks hooks;
    hooks.ready_to_run = [&gate_calls]() {
        ++gate_calls;
        return gate_calls >= 3u; /* paused for two cycles, then resume */
    };
    hooks.sleep_for = [&sleep_calls](std::chrono::milliseconds delay) {
        ++sleep_calls;
        assert(delay == std::chrono::milliseconds(300000)); /* poll interval */
    };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                             drained_feed, DaemonPersistence{}, hooks);

    assert(report.cycles == 1u);
    assert(sleep_calls == 2u); /* two deferred polls, then the bound stops us */
    assert(report.learned == 2u);
}

void test_snapshot_cadence_bounds_graph_writes() {
    const auto dir = scratch_dir("cadence");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 4;
    config.snapshot_every_cycles = 2;

    int generated = 0;
    const auto feed = [&generated](const std::optional<std::string> &) -> SyncPage {
        ++generated;
        const std::string index = std::to_string(generated);
        return SyncPage{
            .items = {obs("at://fixture/u" + index, "text " + index)},
            .next_cursor = std::nullopt,
        };
    };

    unsigned graph_saves = 0;
    DaemonPersistence persistence;
    persistence.save_graph = [&graph_saves]() { ++graph_saves; };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(), feed,
                                             persistence, DaemonHooks{});

    assert(report.cycles == 4u);
    assert(report.learned == 4u);
    assert(report.snapshots_saved == 2u);
    assert(graph_saves == 2u);
}

void test_exit_flushes_pending_snapshot() {
    const auto dir = scratch_dir("exit-flush");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.max_cycles = 3;
    config.snapshot_every_cycles = 2;

    int generated = 0;
    const auto feed = [&generated](const std::optional<std::string> &) -> SyncPage {
        ++generated;
        const std::string index = std::to_string(generated);
        return SyncPage{
            .items = {obs("at://fixture/u" + index, "text " + index)},
            .next_cursor = std::nullopt,
        };
    };

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(), feed,
                                             DaemonPersistence{}, DaemonHooks{});

    assert(report.cycles == 3u);
    /* cycle 2 snapshot + final flush of the dirty cycle 3 */
    assert(report.snapshots_saved == 2u);
}

void test_pages_per_cycle_bounds_each_cycle() {
    const auto dir = scratch_dir("pages");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    DaemonConfig config = no_jitter_config();
    config.pages_per_cycle = 1;
    config.max_cycles = 2;

    const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                             two_page_feed, DaemonPersistence{}, DaemonHooks{});

    assert(report.cycles == 2u);
    assert(report.pages_completed == 2u);
    assert(report.learned == 3u);
    assert(report.exhausted);
}

/* ---------------------------------------------------------------- */
/* Restart resume                                                    */
/* ---------------------------------------------------------------- */

void test_restart_resumes_without_duplicate_training() {
    const auto dir = scratch_dir("restart");
    const auto model_path = dir / "model.bin";
    const auto state_path = dir / "state.json";
    const auto ledger_path = dir / "ledger.bin";

    DaemonConfig config = no_jitter_config();
    config.pages_per_cycle = 1;

    /* Run one cycle, then persist exactly as the CLI daemon does. */
    std::size_t learned_first = 0;
    {
        atperson::LanguageGraph graph;
        atperson::Ledger ledger(ledger_path);
        auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
        config.max_cycles = 1;

        DaemonPersistence persistence;
        persistence.save_ingestion = [&state, &state_path]() {
            atperson::save_ingestion_state(state, state_path);
        };
        persistence.save_graph = [&graph, &model_path]() { graph.save(model_path); };

        const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                                 two_page_feed, persistence, DaemonHooks{});
        learned_first = report.learned;
        assert(learned_first == 2u);
        assert(state.catchup.active); /* interrupted traversal persisted */
    }

    /* Cold restart: reload learned state and the cursor from disk. */
    {
        auto graph = atperson::LanguageGraph::load(model_path);
        atperson::Ledger ledger(ledger_path);
        auto state =
            atperson::load_ingestion_state(state_path, "https://bsky.social", "did:plc:abc");
        assert(state.catchup.active);

        config.max_cycles = 1;
        DaemonPersistence persistence;
        persistence.save_ingestion = [&state, &state_path]() {
            atperson::save_ingestion_state(state, state_path);
        };
        persistence.save_graph = [&graph, &model_path]() { graph.save(model_path); };

        const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                                 two_page_feed, persistence, DaemonHooks{});
        assert(report.learned == 1u); /* the remaining page only */
        assert(report.duplicates == 0u);
        assert(report.exhausted);
        assert(!state.catchup.active);
    }

    /* A third run over the now-drained timeline trains nothing. */
    {
        auto graph = atperson::LanguageGraph::load(model_path);
        atperson::Ledger ledger(ledger_path);
        auto state =
            atperson::load_ingestion_state(state_path, "https://bsky.social", "did:plc:abc");
        config.max_cycles = 1;

        const auto report = atperson::run_daemon(config, graph, ledger, state, default_limits(),
                                                 two_page_feed, DaemonPersistence{}, DaemonHooks{});
        assert(report.learned == 0u);
        assert(report.duplicates == 2u);
        assert(ledger.count() == 3u);
    }
}

} // namespace

int main() {
    test_backoff_grows_deterministically_and_is_bounded();
    test_backoff_jitter_is_deterministic_and_bounded();
    test_failure_classification();
    test_config_defaults_are_valid();
    test_config_rejects_impossible_values();
    test_config_environment_overrides();

    test_repeated_cycles_do_not_relearn();
    test_transient_failure_backs_off_then_recovers();
    test_fatal_error_propagates_and_stops();
    test_graceful_stop_flushes_dirty_state();
    test_stop_before_first_cycle_still_flushes_ingestion();
    test_paused_gate_defers_cycles();
    test_snapshot_cadence_bounds_graph_writes();
    test_exit_flushes_pending_snapshot();
    test_pages_per_cycle_bounds_each_cycle();

    test_restart_resumes_without_duplicate_training();

    std::printf("daemon tests passed\n");
    return 0;
}
