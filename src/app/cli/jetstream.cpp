/* CLI Jetstream public backfill command (#60). */

#include "jetstream.hpp"

#include "atproto/jetstream_client.hpp"
#include "cli/config.hpp"
#include "cli/jetstream_filters.hpp"
#include "ingestion/state.hpp"
#include "journal/store.hpp"
#include "lock.hpp"
#include "engine.hpp"
#include "linkage.hpp"
#include "control/state.hpp"
#include "state/time.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace atperson {
namespace cli {

namespace {

JetstreamLimits parse_limits(int max_events, int max_ms) {
    JetstreamLimits limits;
    limits.max_events = max_events > 0 ? static_cast<std::uint64_t>(max_events) : 0u;
    limits.max_ms = max_ms > 0 ? static_cast<std::int64_t>(max_ms) : 0;
    return limits;
}

std::vector<std::string> jetstream_collections() {
    const std::string path = env_or("ATPERSON_JETSTREAM_COLLECTIONS_FILE");
    if (path.empty()) return {"app.bsky.feed.post"};
    return load_jetstream_collections(path);
}

std::vector<std::string> jetstream_dids() {
    const std::string path = env_or("ATPERSON_JETSTREAM_DIDS_FILE");
    return path.empty() ? std::vector<std::string>{} : load_jetstream_dids(path);
}

} // namespace

int run_jetstream(std::ostream &out, const RuntimeResourceStatus &resource_status,
                  const std::filesystem::path &data_dir, LanguageGraph &graph,
                  const std::filesystem::path &model_path,
                  const std::filesystem::path &ledger_file,
                  const std::filesystem::path &state_file, int max_events,
                  int max_ms,
                  const std::function<void(const LanguageGraph &)> &print_stats) {
    /* Operator pause gate (#22): refuse new ingestion work while paused. */
    const auto control_file = cli::control_state_path();
    auto control = load_control_state(control_file);
    if (control.paused) {
        throw std::runtime_error(
            "jetstream refused: runtime is paused (atperson control resume)");
    }

    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    atperson::Ledger ledger(ledger_file);

    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    /* The Jetstream backfill is unauthenticated: it ingests public records
     * without a session, so the DID field is empty rather than a DID. */
    auto ingestion =
        atperson::load_ingestion_state(state_file, service, "",
                                       kSourceKindJetstream);

    const JetstreamLimits limits = parse_limits(max_events, max_ms);
    const std::string endpoint =
        env_or("ATPERSON_JETSTREAM_ENDPOINT",
               "wss://jetstream.us-east.bsky.network/subscribe");
    const std::string self_did = required_env("ATPERSON_SELF_DID");
    if (self_did.rfind("did:", 0u) != 0u) {
        throw std::runtime_error("ATPERSON_SELF_DID must be a DID");
    }
    atperson::JetstreamClient client(endpoint, jetstream_collections(), jetstream_dids());

    const auto linker = atperson::make_journal_linker(cli::action_journal_path());

    atperson::JetstreamRunResult result;
    for (;;) {
        result = atperson::run_jetstream_backfill(graph, ledger, ingestion, client, limits,
                                                  linker, self_did);
        if (result.exhausted) {
            break;
        }
        const std::uint32_t delay_ms = client.reconnect_after_ms();
        if (delay_ms == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    graph.save(model_path);
    ingestion.checkpoint.generation++;
    atperson::save_ingestion_state(ingestion, state_file);
    control.last_sync_at = atperson::control_now_rfc3339();
    atperson::save_control_state(control, control_file);

    out << "jetstream: " << result.events_consumed << " frame(s), "
        << result.observations_seen << " observation(s)"
        << (result.exhausted ? ", feed exhausted" : ", catch-up pending")
        << "; learned from " << result.learned << " (skipped " << result.skipped
        << ", duplicate " << result.duplicates << ") public post records\n";
    print_stats(graph);
    return 0;
}

} // namespace cli
} // namespace atperson
