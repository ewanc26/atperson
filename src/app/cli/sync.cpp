// CLI network sync command: sync [max-pages].
//
// Implementation of the contract in sync.hpp. The body is moved
// byte-faithfully from the original single-file dispatch in src/app/main.cpp;
// behaviour, ordering and output text are unchanged. The per-page resource
// refresh lambda keeps its original capture set so the moved statements stay
// recognisable.

#include "sync.hpp"

#include "client.hpp"
#include "config.hpp"
#include "ingestion/state.hpp"
#include "journal/store.hpp"
#include "lock.hpp"
#include "engine.hpp"
#include "linkage.hpp"
#include "control/state.hpp"
#include "parallel.hpp"
#include "worker/pool.hpp"

#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {

namespace {

/* Parallel sync is opt-in: it overlaps page fetches with observation
 * processing on a pool sized from the resource budget. It is off by default
 * so the sequential path remains the reference for tests and CI. The pool
 * is fail-closed: a fetch failure aborts the run without touching the cursor
 * or the ledger. */
bool parallel_sync_enabled() {
    const char *value = std::getenv("ATPERSON_SYNC_PARALLEL");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

} // namespace

int run_sync(std::ostream &out, std::ostream &err, const RuntimeResourceStatus &resource_status,
             const std::filesystem::path &data_dir, LanguageGraph &graph,
             const std::filesystem::path &model_path,
             const std::filesystem::path &ledger_file,
             const std::filesystem::path &state_file, int max_pages,
             const std::function<void(const LanguageGraph &)> &print_stats) {
    /* Operator pause gate (#22): refuse new ingestion work while paused.
     * Learned state is untouched; the operator resumes explicitly. */
    const auto control_file = atperson::cli::control_state_path();
    auto control = atperson::load_control_state(control_file);
    if (control.paused) {
        throw std::runtime_error(
            "sync refused: runtime is paused (atperson control resume)");
    }

    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    atperson::AtprotoClient client(service, required_env("ATPERSON_IDENTIFIER"),
                                    required_env("ATPERSON_APP_PASSWORD"));
    atperson::Ledger ledger(ledger_file);

    auto ingestion =
        atperson::load_ingestion_state(state_file, service, client.account_did());

    auto mutable_status = resource_status;
    atperson::SyncLimits limits;
    limits.page_size = mutable_status.budget.sync_page_size;
    limits.max_pages = max_pages;
    limits.max_observations = mutable_status.budget.sync_max_observations;
    limits.max_observations = std::min<std::uint64_t>(
        limits.max_observations,
        static_cast<std::uint64_t>(mutable_status.budget.neural_runtime.observation_work_batch));

    const auto resource_paths = atperson::cli::durable_paths();
    const auto resource_overrides = atperson::resource_overrides_from_environment();
    const auto fetch_page = [&client, &limits, &graph, &mutable_status, &resource_overrides,
                             &resource_paths,
                             &err](const std::optional<std::string> &cursor) {
        mutable_status = atperson::refresh_runtime_resources(graph, resource_paths,
                                                             resource_overrides);
        atperson::require_runtime_write_headroom(mutable_status);
        limits.page_size = mutable_status.budget.sync_page_size;
        limits.max_observations = mutable_status.budget.sync_max_observations;
        limits.max_observations = std::min<std::uint64_t>(
            limits.max_observations,
            static_cast<std::uint64_t>(
                mutable_status.budget.neural_runtime.observation_work_batch));
        try {
            return client.fetch_timeline_page(cursor, limits.page_size);
        } catch (const atperson::TimelineHttpError &) {
            if (cursor) {
                err << "atperson: saved cursor rejected by the service; "
                       "resetting to the timeline head\n";
                return client.fetch_timeline_page(std::nullopt, limits.page_size);
            }
            throw;
        }
    };

    const auto linker = atperson::make_journal_linker(atperson::cli::action_journal_path());

    atperson::SyncResult result;
    if (parallel_sync_enabled() &&
        mutable_status.budget.neural_runtime.surrounding_worker_allowance > 0u) {
        /* The pool is sized from the effective CPU capacity, so a container
         * with a fractional quota gets fewer workers than the host has
         * logical CPUs. One worker reproduces the sequential path through
         * the queue, so the result is identical for fixed inputs. */
        const auto pool_config = atperson::WorkerPool::config_from_system(
            mutable_status.system,
            mutable_status.budget.neural_runtime.surrounding_worker_allowance);
        atperson::WorkerPool pool(pool_config);
        result = atperson::run_sync_parallel(graph, ledger, ingestion, fetch_page, limits,
                                             pool, linker);
        pool.shutdown();
    } else {
        result = atperson::run_sync(graph, ledger, ingestion, fetch_page, limits, linker);
    }

    graph.save(model_path);
    ingestion.checkpoint.generation++;
    atperson::save_ingestion_state(ingestion, state_file);
    control.last_sync_at = atperson::control_now_rfc3339();
    atperson::save_control_state(control, control_file);
    out << "completed " << result.pages_completed << " page(s), " << result.observations_seen
        << " observation(s)" << (result.exhausted ? ", timeline exhausted" : ", catch-up pending")
        << "; learned from " << result.learned << " (skipped " << result.skipped
        << ", duplicate " << result.duplicates << ") public timeline posts\n";
    print_stats(graph);
    return 0;
}

} // namespace cli
} // namespace atperson
