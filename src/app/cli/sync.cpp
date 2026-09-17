// CLI network sync command: sync [max-pages].
//
// Implementation of the contract in sync.hpp. The body is moved
// byte-faithfully from the original single-file dispatch in src/app/main.cpp;
// behaviour, ordering and output text are unchanged. The per-page resource
// refresh lambda keeps its original capture set so the moved statements stay
// recognisable.

#include "sync.hpp"

#include "atproto_client.hpp"
#include "config.hpp"
#include "ingestion_state.hpp"
#include "state_lock.hpp"
#include "sync_engine.hpp"

#include <iostream>
#include <optional>
#include <ostream>
#include <string>

namespace atperson {
namespace cli {

int run_sync(std::ostream &out, std::ostream &err, const RuntimeResourceStatus &resource_status,
             const std::filesystem::path &data_dir, LanguageGraph &graph,
             const std::filesystem::path &model_path,
             const std::filesystem::path &ledger_file,
             const std::filesystem::path &state_file, int max_pages,
             const std::function<void(const LanguageGraph &)> &print_stats) {
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

    const auto result = atperson::run_sync(graph, ledger, ingestion, fetch_page, limits);
    graph.save(model_path);
    ingestion.checkpoint.generation++;
    atperson::save_ingestion_state(ingestion, state_file);
    out << "completed " << result.pages_completed << " page(s), " << result.observations_seen
        << " observation(s)" << (result.exhausted ? ", timeline exhausted" : ", catch-up pending")
        << "; learned from " << result.learned << " (skipped " << result.skipped
        << ", duplicate " << result.duplicates << ") public timeline posts\n";
    print_stats(graph);
    return 0;
}

} // namespace cli
} // namespace atperson
