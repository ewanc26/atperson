#include "daemon.hpp"

#include "client.hpp"
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

#include <chrono>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {

int run_daemon_command(std::ostream &out, std::ostream &err,
                       const RuntimeResourceStatus &resource_status,
                       const std::filesystem::path &data_dir, LanguageGraph &graph,
                       const std::filesystem::path &model_path,
                       const std::filesystem::path &ledger_file,
                       const std::filesystem::path &state_file, int max_cycles_override,
                       const std::function<void(const LanguageGraph &)> &print_stats) {
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
    auto ingestion = load_ingestion_state(state_file, service, client.account_did());

    auto mutable_status = resource_status;
    SyncLimits limits;
    limits.page_size = mutable_status.budget.sync_page_size;
    limits.max_pages = config.pages_per_cycle;
    limits.max_observations = mutable_status.budget.sync_max_observations;

    const auto resource_paths = durable_paths();
    const auto resource_overrides = resource_overrides_from_environment();
    const auto fetch_page = [&client, &limits, &graph, &mutable_status, &resource_overrides,
                             &resource_paths, &err](const std::optional<std::string> &cursor) {
        mutable_status = refresh_runtime_resources(graph, resource_paths, resource_overrides);
        require_runtime_write_headroom(mutable_status);
        limits.page_size = mutable_status.budget.sync_page_size;
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

    const DaemonPersistence persistence{
        [&ingestion, &state_file]() {
            ingestion.checkpoint.generation++;
            save_ingestion_state(ingestion, state_file);
        },
        [&graph, &model_path]() { graph.save(model_path); },
    };

    unsigned long long cycle_number = 0;

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
        [&out, &cycle_number](const SyncResult &result) {
            ++cycle_number;
            out << "daemon: cycle " << cycle_number << ": " << result.pages_completed
                << " page(s), " << result.observations_seen << " observation(s), learned "
                << result.learned << " (skipped " << result.skipped << ", duplicate "
                << result.duplicates << ")"
                << (result.exhausted ? ", timeline exhausted" : ", catch-up pending") << '\n';
        },
        [&err](std::chrono::milliseconds delay, const RetryableError &error) {
            err << "atperson: transient ingestion failure (" << error.what() << "); retrying in "
                << delay.count() << "ms\n";
        },
    };

    const DaemonRunReport report =
        run_daemon(config, graph, ledger, ingestion, limits, fetch_page, persistence, hooks,
                   make_journal_linker(action_journal_path()));

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
    return 0;
}

} // namespace cli
} // namespace atperson
