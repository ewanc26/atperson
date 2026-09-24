#include "daemon.hpp"
#include "autonomy/run_state.hpp"

#include "client.hpp"
#include "atproto/jetstream_replay_client.hpp"
#include "atproto/session.hpp"
#include "atproto/jetstream_filter.hpp"
#include "atproto/writer.hpp"
#include "config.hpp"
#include "control/state.hpp"
#include "daemon/config.hpp"
#include "daemon/failure.hpp"
#include "daemon/loop.hpp"
#include "daemon/signals.hpp"
#include "engine.hpp"
#include "ingestion/state.hpp"
#include "linkage.hpp"
#include "lock.hpp"
#include "parallel.hpp"
#include "scheduler/cycle.hpp"
#include "worker/pool.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {

namespace {

/* Parallel sync is opt-in; see run_sync. The daemon builds one pool for its
 * whole lifetime and reuses it across cycles, so the pool's worker threads
 * survive a cycle boundary. The pool is fail-closed: a fetch failure aborts
 * the cycle and the daemon backs off rather than touching the cursor or the
 * ledger. */
bool parallel_sync_enabled() {
    const char *value = std::getenv("ATPERSON_SYNC_PARALLEL");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

std::uint64_t daemon_archive_sequence(const char *name) {
    const std::string value = env_or(name);
    if (value.empty()) {
        throw std::runtime_error(std::string("missing ") + name);
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) throw std::invalid_argument("trailing");
        return parsed;
    } catch (const std::exception &) {
        throw std::runtime_error(std::string(name) + " must be a decimal sequence");
    }
}

void run_startup_archive_if_configured(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    LanguageGraph &graph,
    const std::filesystem::path &model_path, Ledger &ledger,
    const SyncLinker &linker, protocol::EvidenceLedger *protocol_ledger) {
    const std::string after_value = env_or("ATPERSON_DAEMON_ARCHIVE_AFTER");
    const std::string span_value = env_or("ATPERSON_DAEMON_ARCHIVE_SPAN");
    if (after_value.empty() && span_value.empty()) return;
    if (!after_value.empty() && !span_value.empty()) {
        throw std::runtime_error(
            "ATPERSON_DAEMON_ARCHIVE_SPAN cannot be combined with "
            "ATPERSON_DAEMON_ARCHIVE_AFTER");
    }
    std::optional<std::uint64_t> relative_span;
    std::uint64_t after = 0u;
    if (!span_value.empty()) {
        const std::uint64_t span = daemon_archive_sequence(
            "ATPERSON_DAEMON_ARCHIVE_SPAN");
        if (span == 0u || span > kJetstreamArchiveMaxSequenceSpan) {
            throw std::runtime_error(
                "ATPERSON_DAEMON_ARCHIVE_SPAN must be between 1 and the "
                "10,000,000 sequence cap");
        }
        relative_span = span;
    } else {
        after = daemon_archive_sequence("ATPERSON_DAEMON_ARCHIVE_AFTER");
    }
    const std::string before_value = env_or("ATPERSON_DAEMON_ARCHIVE_BEFORE");
    std::optional<std::uint64_t> before;
    if (!before_value.empty()) before = daemon_archive_sequence("ATPERSON_DAEMON_ARCHIVE_BEFORE");
    if (!relative_span) {
        if (!before) {
            if (after > std::numeric_limits<std::uint64_t>::max() -
                           kJetstreamArchiveMaxSequenceSpan) {
                throw std::runtime_error(
                    "ATPERSON_DAEMON_ARCHIVE_AFTER is too large for the default window");
            }
            before = after + kJetstreamArchiveMaxSequenceSpan;
        }
        if (before && *before <= after) {
            throw std::runtime_error(
                "ATPERSON_DAEMON_ARCHIVE_BEFORE must be greater than AFTER");
        }
        if (*before - after > kJetstreamArchiveMaxSequenceSpan) {
            throw std::runtime_error(
                "daemon archive window exceeds the 10,000,000 sequence cap");
        }
    }
    require_runtime_write_headroom(resource_status);
    const std::string endpoint = env_or(
        "ATPERSON_JETSTREAM_ENDPOINT", kDefaultJetstreamEndpoint);
    auto archive_state = load_ingestion_state(
        jetstream_state_path(), endpoint, "", kSourceKindJetstream);
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                           required_env("ATPERSON_APP_PASSWORD"));
    JetstreamReplayClient replay(
        *session.agent(), required_env("ATPERSON_JETSTREAM_ARCHIVE_TOKEN"));
    const auto result = atperson::run_jetstream_archive(
        graph, ledger, archive_state, replay, after, before, relative_span,
        required_self_did(),
        jetstream_collections_path().empty()
            ? default_jetstream_collections()
            : load_jetstream_collections(jetstream_collections_path()),
        jetstream_dids_path().empty()
            ? std::vector<std::string>{}
            : load_jetstream_dids(jetstream_dids_path()),
        linker, protocol_ledger);
    graph.save(model_path);
    archive_state.checkpoint.generation++;
    save_ingestion_state(archive_state, jetstream_state_path());
    out << "daemon: archive startup phase consumed " << result.events_consumed
        << " event(s), learned " << result.learned << " (skipped " << result.skipped
        << ", duplicate " << result.duplicates << ")"
        << (result.exhausted ? ", sealed archive exhausted" : ", window incomplete") << '\n';
}

/* One autonomous scheduler cycle (#140) after a successful perception
 * cycle. Off unless ATPERSON_SCHEDULER=1. The writer is established lazily
 * inside the attempt atom, so a cycle with no approved proposal never
 * reads credentials or touches the network. */
SchedulerCycleReport run_scheduler_after_cycle(const std::filesystem::path &data_dir,
                                               const SchedulerConfig &config,
                                               const LanguageGraph &graph, const Ledger &ledger,
                                               std::int64_t now) {
    if (!config.enabled) {
        return {};
    }
    std::unique_ptr<WolframSession> session;
    std::unique_ptr<WolframWriter> writer;
    const auto writer_for = [&]() -> OutboundWriter & {
        if (!session) {
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                       required_env("ATPERSON_APP_PASSWORD"));
            writer = std::make_unique<WolframWriter>(*session);
        }
        return *writer;
    };
    const auto steady_ms = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const SchedulerCycle cycle{
        data_dir,
        scheduler_proposals_path(),
        OutboundAttemptPaths{outbound_policy_path(), outbound_budget_path(),
                             control_state_path(), outbound_audit_path(), action_journal_path(),
                             authorization_envelopes_path()},
        writer_for,
        now,
        steady_ms};
    const SchedulerCycleReport report =
        run_scheduler_cycle(config, cycle, graph, ledger);
    return report;
}

void print_scheduler_report(std::ostream &out, const SchedulerCycleReport &report) {
    out << "scheduler: " << report.contexts_examined << " context(s), "
        << report.decisions << " decision(s), " << report.abstentions << " abstention(s), "
        << report.proposals_written << " proposal(s) written, " << report.executions_attempted
        << " execution attempt(s): " << report.executed << " executed, " << report.refused
        << " refused, " << report.failed << " failed — " << report.detail << '\n';
}

} // namespace

int run_daemon_command(std::ostream &out, std::ostream &err,
                       const RuntimeResourceStatus &resource_status,
                       const std::filesystem::path &data_dir, LanguageGraph &graph,
                       const std::filesystem::path &model_path,
                       const std::filesystem::path &ledger_file,
                       const std::filesystem::path &state_file, int max_cycles_override,
                       const std::function<void(const LanguageGraph &)> &print_stats) {
    const auto run_state_file = autonomy_run_state_path();
    auto run_state = load_autonomy_run_state(run_state_file);
    run_state.run_id = control_now_rfc3339();
    run_state.phase = AutonomyPhase::Recovering;
    run_state.checkpoint++;
    run_state.last_at = run_state.run_id;
    run_state.detail = "daemon recovery started; AT Protocol timeline only";
    save_autonomy_run_state(run_state, run_state_file);
    DaemonConfig config = daemon_config_from_environment();
    if (max_cycles_override > 0) {
        config.max_cycles = static_cast<std::uint64_t>(max_cycles_override);
        validate_daemon_config(config);
    }
    const auto control_file = control_state_path();

    /* Startup gates: an operator pause refuses a new daemon outright, and an
     * unsafe disk/memory state refuses before any durable write. */
    if (load_control_state(control_file).paused) {
        throw std::runtime_error("daemon refused: runtime is paused (atperson control resume)");
    }
    require_runtime_write_headroom(resource_status);

    install_shutdown_signals();

    /* Single-writer ownership for the daemon's whole lifetime: another
     * process must not load a stale in-memory snapshot and later clobber
     * this one. */
    const StateLock writer_lock(data_dir);
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    AtprotoClient client(service, required_env("ATPERSON_IDENTIFIER"),
                         required_env("ATPERSON_APP_PASSWORD"));
    Ledger ledger(ledger_file);
    const auto linker = make_journal_linker(action_journal_path());
    protocol::EvidenceLedger protocol_ledger_file(protocol_ledger_path());
    run_startup_archive_if_configured(
        out, resource_status, graph, model_path, ledger, linker, &protocol_ledger_file);
    run_state.phase = AutonomyPhase::Learning;
    run_state.checkpoint++;
    run_state.last_at = control_now_rfc3339();
    run_state.detail = "bounded AT Protocol perception cycle ready";
    save_autonomy_run_state(run_state, run_state_file);
    auto ingestion = load_ingestion_state(state_file, service, client.account_did());

    auto mutable_status = resource_status;
    SyncLimits limits;
    limits.page_size = std::min(
        mutable_status.budget.sync_page_size,
        static_cast<int>(mutable_status.neural_runtime.observation_work_batch));
    limits.max_pages = config.pages_per_cycle;
    limits.max_observations = mutable_status.budget.sync_max_observations;

    const auto resource_paths = durable_paths();
    const auto resource_overrides = resource_overrides_from_environment();
    const auto fetch_page = [&client, &limits, &graph, &mutable_status, &resource_overrides,
                             &resource_paths, &err](const std::optional<std::string> &cursor) {
        mutable_status = refresh_runtime_resources(graph, resource_paths, resource_overrides);
        require_runtime_write_headroom(mutable_status);
        limits.page_size = std::min(
            mutable_status.budget.sync_page_size,
            static_cast<int>(
                mutable_status.neural_runtime.observation_work_batch));
        limits.max_observations = mutable_status.budget.sync_max_observations;
        try {
            return client.fetch_timeline_page(cursor, limits.page_size);
        } catch (const TimelineHttpError &error) {
            if (cursor) {
                err << "atperson: saved cursor rejected by the service; "
                       "resetting to the timeline head\n";
                try {
                    return client.fetch_timeline_page(std::nullopt, limits.page_size);
                } catch (const TimelineHttpError &reset_error) {
                    throw RetryableError(reset_error.what());
                }
            }
            /* Transport failures are transient: back off and retry rather
             * than stop the daemon. Durable state is untouched. */
            throw RetryableError(error.what());
        }
    };

    /* One pool for the daemon's whole lifetime, sized from the effective CPU
     * capacity. Reused across cycles so worker threads are not recreated
     * per cycle. The pool is fail-closed: a fetch failure aborts the cycle
     * and the daemon backs off rather than touching the cursor or the
     * ledger. */
    atperson::WorkerPool *pool = nullptr;
    if (parallel_sync_enabled() &&
        mutable_status.neural_runtime.surrounding_worker_threads != 0u) {
        const auto pool_config = atperson::WorkerPool::config_from_system(
            mutable_status.system,
            mutable_status.neural_runtime.surrounding_worker_threads);
        pool = new atperson::WorkerPool(pool_config);
    }

    /* The loop is sync-runner-agnostic: it does not know whether a cycle
     * fetches sequentially or in parallel. The runner captures the fetcher,
     * the graph, the ledger and the ingestion state by reference. */
    atperson::SyncRunner run_cycle = [&](LanguageGraph &g, atperson::Ledger &l,
                                         atperson::IngestionState &s,
                                         const atperson::SyncLimits &cl,
                                         const atperson::SyncLinker &ln) -> atperson::SyncResult {
        if (pool != nullptr) {
            /* Re-sample before each bounded cycle. `run_sync_parallel` drains
             * the pool before returning, so this resize cannot reorder a
             * fetched page or race the single C23 learning owner. */
            mutable_status = refresh_runtime_resources(g, resource_paths, resource_overrides);
            const auto pool_config = atperson::WorkerPool::config_from_system(
                mutable_status.system,
                std::max<std::size_t>(
                    1u, mutable_status.neural_runtime.surrounding_worker_threads));
            pool->resize(pool_config);
            return atperson::run_sync_parallel(g, l, s, fetch_page, cl, *pool, ln);
        }
        return atperson::run_sync(g, l, s, fetch_page, cl, ln);
    };

    const DaemonPersistence persistence{
        [&ingestion, &state_file]() {
            ingestion.checkpoint.generation++;
            save_ingestion_state(ingestion, state_file);
        },
        [&graph, &model_path]() { graph.save(model_path); },
    };

    unsigned long long cycle_number = 0;
    const SchedulerConfig scheduler_config = scheduler_config_from_environment();
    std::uint64_t scheduler_cycles = 0;
    std::uint64_t scheduler_decisions = 0;
    std::uint64_t scheduler_abstentions = 0;
    std::uint64_t scheduler_executed = 0;

    const DaemonHooks hooks{
        [](std::chrono::milliseconds delay) { static_cast<void>(sleep_until_interrupted(delay)); },
        [&control_file]() {
            if (shutdown_requested()) {
                return true;
            }
            try {
                return load_control_state(control_file).shutdown_requested_at.has_value();
            } catch (const std::exception &) {
                /* An unreadable control file must not silently stop the
                 * daemon; the startup gate already validated it. */
                return false;
            }
        },
        [&control_file]() {
            try {
                return !load_control_state(control_file).paused;
            } catch (const std::exception &) {
                return true;
            }
        },
        [&out, &data_dir, &cycle_number, &scheduler_config, &graph, &ledger, &scheduler_cycles,
         &scheduler_decisions, &scheduler_abstentions,
         &scheduler_executed](const SyncResult &result) {
            ++cycle_number;
            out << "daemon: cycle " << cycle_number << ": " << result.pages_completed
                << " page(s), " << result.observations_seen << " observation(s), learned "
                << result.learned << " (skipped " << result.skipped << ", duplicate "
                << result.duplicates << ")"
                << (result.exhausted ? ", timeline exhausted" : ", catch-up pending") << '\n';
            if (scheduler_config.enabled) {
                ++scheduler_cycles;
                const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
                const SchedulerCycleReport scheduler_report =
                    run_scheduler_after_cycle(data_dir, scheduler_config, graph, ledger, now);
                scheduler_decisions += scheduler_report.decisions;
                scheduler_abstentions += scheduler_report.abstentions;
                scheduler_executed += scheduler_report.executed;
                print_scheduler_report(out, scheduler_report);
            }
        },
        [&err](std::chrono::milliseconds delay, const RetryableError &error) {
            err << "atperson: transient ingestion failure (" << error.what() << "); retrying in "
                << delay.count() << "ms\n";
        },
    };

    DaemonRunReport report;
    try {
        report = run_daemon(config, graph, ledger, ingestion, limits, run_cycle, persistence,
                            hooks, linker);
    } catch (...) {
        if (pool != nullptr) {
            pool->shutdown();
            delete pool;
        }
        throw;
    }

    if (pool != nullptr) {
        pool->shutdown();
        delete pool;
    }

    if (report.stopped) {
        out << "daemon: shutdown requested; durable state flushed\n";
    } else if (report.bounded) {
        out << "daemon: reached configured max cycles\n";
    }
    out << "daemon: " << report.cycles << " cycle(s), " << report.observations_seen
        << " observation(s); learned " << report.learned << " (skipped " << report.skipped
        << ", duplicate " << report.duplicates << "); transient failures "
        << report.transient_failures << ", snapshots " << report.snapshots_saved << '\n';
    print_stats(graph);
    run_state.phase = AutonomyPhase::Stopped;
    run_state.checkpoint++;
    run_state.last_at = control_now_rfc3339();
    if (scheduler_config.enabled) {
        run_state.detail = "daemon stopped; scheduler cycles " +
                           std::to_string(scheduler_cycles) + ", decisions " +
                           std::to_string(scheduler_decisions) + ", abstentions " +
                           std::to_string(scheduler_abstentions) + ", executed " +
                           std::to_string(scheduler_executed);
    } else {
        run_state.detail = "daemon stopped after local checkpoint";
    }
    save_autonomy_run_state(run_state, run_state_file);
    return 0;
}

} // namespace cli
} // namespace atperson
