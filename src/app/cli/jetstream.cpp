/* CLI Jetstream public backfill command (#60). */

#include "jetstream.hpp"

#include "atproto/jetstream_client.hpp"
#include "atproto/jetstream_filter.hpp"
#include "atproto/jetstream_replay_client.hpp"
#include "atproto/session.hpp"
#include "cli/config.hpp"
#include "ingestion/state.hpp"
#include "journal/store.hpp"
#include "lock.hpp"
#include "engine.hpp"
#include "linkage.hpp"
#include "control/state.hpp"
#include "state/time.hpp"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <optional>

namespace atperson {
namespace cli {

namespace {

JetstreamLimits parse_limits(int max_events, int max_ms) {
    JetstreamLimits limits;
    limits.max_events = max_events > 0 ? static_cast<std::uint64_t>(max_events) : 0u;
    limits.max_ms = max_ms > 0 ? static_cast<std::int64_t>(max_ms) : 0;
    return limits;
}

} // namespace

int run_jetstream_status(
    std::ostream &out, const std::filesystem::path &state_file,
    const std::filesystem::path &collections_file,
    const std::filesystem::path &dids_file) {
    const std::string endpoint = env_or(
        "ATPERSON_JETSTREAM_ENDPOINT",
        "wss://jetstream.us-east.bsky.network/subscribe");
    const std::string configured_self = self_did();
    const std::vector<std::string> collections =
        collections_file.empty()
            ? atperson::default_jetstream_collections()
            : atperson::load_jetstream_collections(collections_file);
    const std::vector<std::string> dids =
        dids_file.empty() ? std::vector<std::string>{}
                          : atperson::load_jetstream_dids(dids_file);
    const IngestionState state = load_ingestion_state(
        state_file, endpoint, "", kSourceKindJetstream);
    const std::string archive_after = env_or("ATPERSON_DAEMON_ARCHIVE_AFTER");
    const std::string archive_before = env_or("ATPERSON_DAEMON_ARCHIVE_BEFORE");

    out << "jetstream endpoint: " << endpoint << '\n'
        << "self DID: " << (configured_self.empty() ? "missing (live ingestion will refuse)" : configured_self) << '\n'
        << "phase: "
        << (archive_after.empty()
                ? "live-only (archive replay available; daemon startup window not configured)"
                : "archive-startup (daemon will replay before timeline cycles)")
        << '\n'
        << "state: " << state_file.string() << '\n'
        << "archive after: " << (archive_after.empty() ? "none" : archive_after) << '\n'
        << "archive before: " << (archive_before.empty() ? "none" : archive_before) << '\n'
        << "cursor: ";
    if (state.catchup.active && state.catchup.cursor) {
        out << *state.catchup.cursor;
    } else {
        out << "none";
    }
    out << '\n'
        << "checkpoint generation: " << state.checkpoint.generation << '\n'
        << "collections (" << collections.size() << "):";
    for (const std::string &collection : collections) {
        out << "\n  " << collection;
    }
    out << "\nDID filters (" << dids.size() << "):";
    if (dids.empty()) {
        out << " none";
    } else {
        for (const std::string &did : dids) {
            out << "\n  " << did;
        }
    }
    out << '\n';
    return 0;
}

int run_jetstream_archive(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    const std::filesystem::path &data_dir, LanguageGraph &graph,
    const std::filesystem::path &model_path,
    const std::filesystem::path &ledger_file,
    const std::filesystem::path &state_file,
    std::optional<std::uint64_t> after_seq,
    std::optional<std::uint64_t> before_seq,
    const std::function<void(const LanguageGraph &)> &print_stats) {
    const auto control_file = cli::control_state_path();
    if (load_control_state(control_file).paused) {
        throw std::runtime_error(
            "jetstream archive refused: runtime is paused (atperson control resume)");
    }
    require_runtime_write_headroom(resource_status);
    const StateLock writer_lock(data_dir);
    Ledger ledger(ledger_file);
    const std::string endpoint = env_or(
        "ATPERSON_JETSTREAM_ENDPOINT",
        "wss://jetstream.us-east.bsky.network/subscribe");
    auto state = load_ingestion_state(state_file, endpoint, "", kSourceKindJetstream);
    if (!after_seq) {
        if (state.catchup.active && state.catchup.cursor) {
            try {
                after_seq = std::stoull(*state.catchup.cursor);
            } catch (const std::exception &) {
                throw std::runtime_error(
                    "jetstream archive: persisted cursor is not a decimal sequence; reset it before replay");
            }
        } else {
            after_seq = 0u;
        }
    }
    if (before_seq && *before_seq <= *after_seq) {
        throw std::runtime_error(
            "jetstream archive: before sequence must be greater than after sequence");
    }
    if (!before_seq) {
        if (*after_seq > std::numeric_limits<std::uint64_t>::max() -
                           kJetstreamArchiveMaxSequenceSpan) {
            throw std::runtime_error("jetstream archive: after sequence is too large for the default window");
        }
        before_seq = *after_seq + kJetstreamArchiveMaxSequenceSpan;
    }
    if (*before_seq - *after_seq > kJetstreamArchiveMaxSequenceSpan) {
        throw std::runtime_error("jetstream archive: requested window exceeds the 10,000,000 sequence cap");
    }
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                           required_env("ATPERSON_APP_PASSWORD"));
    JetstreamReplayClient client(*session.agent());
    const auto linker = make_journal_linker(cli::action_journal_path());
    const auto result = atperson::run_jetstream_archive(
        graph, ledger, state, client, *after_seq, before_seq, required_self_did(), linker);
    graph.save(model_path);
    state.checkpoint.generation++;
    save_ingestion_state(state, state_file);
    auto control = load_control_state(control_file);
    control.last_sync_at = control_now_rfc3339();
    save_control_state(control, control_file);
    out << "jetstream archive: " << result.events_consumed << " event(s), "
        << result.observations_seen << " observation(s), learned " << result.learned
        << " (skipped " << result.skipped << ", duplicate " << result.duplicates
        << "); " << (result.exhausted ? "sealed archive exhausted" : "window incomplete")
        << '\n';
    print_stats(graph);
    return 0;
}

int run_jetstream(std::ostream &out, const RuntimeResourceStatus &resource_status,
                  const std::filesystem::path &data_dir, LanguageGraph &graph,
                  const std::filesystem::path &model_path,
                  const std::filesystem::path &ledger_file,
                  const std::filesystem::path &state_file,
                  const std::filesystem::path &collections_file,
                  const std::filesystem::path &dids_file,
                  int max_events, int max_ms,
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

    const std::string endpoint = env_or(
        "ATPERSON_JETSTREAM_ENDPOINT",
        "wss://jetstream.us-east.bsky.network/subscribe");

    const std::string configured_self = required_self_did();

    /*
     * Bind the persisted cursor to the actual Jetstream endpoint, not the
     * authenticated PDS/service URL used by timeline sync. A cursor from one
     * Jetstream server is not assumed to be meaningful on another.
     */
    auto ingestion =
        atperson::load_ingestion_state(state_file, endpoint, "",
                                       kSourceKindJetstream);

    const JetstreamLimits limits = parse_limits(max_events, max_ms);
    const std::vector<std::string> collections =
        collections_file.empty()
            ? atperson::default_jetstream_collections()
            : atperson::load_jetstream_collections(collections_file);
    const std::vector<std::string> dids =
        dids_file.empty() ? std::vector<std::string>{}
                          : atperson::load_jetstream_dids(dids_file);
    const std::string initial_cursor =
        ingestion.catchup.cursor.value_or(std::string{});
    atperson::JetstreamClient client(
        endpoint, configured_self, collections, dids, initial_cursor);

    const auto linker = atperson::make_journal_linker(cli::action_journal_path());

    atperson::JetstreamRunResult result;
    for (;;) {
        result = atperson::run_jetstream_backfill(graph, ledger, ingestion, client, limits,
                                                  linker);
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
        << result.malformed_frames << " malformed skipped, "
        << result.observations_seen << " observation(s)"
        << ", " << result.withdrawn << " withdrawal(s)"
        << (result.reconciled ? ", reconciled" : ", no reconciliation")
        << (result.exhausted ? ", feed exhausted" : ", catch-up pending")
        << "; learned from " << result.learned << " (skipped " << result.skipped
        << ", duplicate " << result.duplicates << "); collections "
        << collections.size() << ", DID filters " << dids.size() << "\n";
    print_stats(graph);
    return 0;
}

} // namespace cli
} // namespace atperson
