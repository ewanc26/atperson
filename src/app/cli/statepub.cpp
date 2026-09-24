#include "statepub.hpp"

#include "../atproto/session.hpp"
#include "../atproto/writer.hpp"
#include "../control/state.hpp"
#include "../replicate/file_writer.hpp"
#include "../replicate/publish.hpp"
#include "../state/lock.hpp"
#include "config.hpp"
#include "journal/store.hpp"
#include "thought/store.hpp"

#include <cstdint>
#include <ostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {
namespace {

constexpr const char *kStatepubLockName = ".statepub-lock";

std::filesystem::path replicate_cursor_path(const std::filesystem::path &data_dir) {
    return data_dir / "replicate" / "cursor.json";
}

} // namespace

int run_statepub_status(std::ostream &out, std::ostream &err,
                        const RuntimeResourceStatus &resource_status,
                        const std::filesystem::path &data_dir,
                        const std::filesystem::path &ledger_file,
                        const std::filesystem::path &journal_file,
                        const std::filesystem::path &thoughts_file) {
    (void)resource_status;
    try {
        const ReplicateCursor cursor = load_replicate_cursor(replicate_cursor_path(data_dir));
        const JournalContents journal = load_journal(journal_file);
        std::uint64_t ledger_committed = 0u;
        {
            const Ledger ledger(ledger_file);
            const std::uint64_t count = ledger.count();
            for (std::uint64_t id = 1u; id <= count; ++id) {
                atp_ledger_entry entry{};
                if (atp_ledger_entry_at(ledger.handle(), static_cast<std::size_t>(id - 1u),
                                        &entry) == ATP_OK &&
                    entry.outcome != ATP_LEDGER_OUTCOME_PENDING) {
                    ++ledger_committed;
                }
            }
        }
        const std::uint64_t observations_pending =
            ledger_committed >= cursor.next_observation_id - 1u
                ? ledger_committed - (cursor.next_observation_id - 1u)
                : 0u;
        out << "observations: committed=" << ledger_committed
            << " published=" << (cursor.next_observation_id - 1u)
            << " backlog=" << observations_pending << '\n';
        out << "actions: total=" << journal.actions.size()
            << " published=" << cursor.actions_published
            << " backlog=" << (journal.actions.size() > cursor.actions_published
                                   ? journal.actions.size() - cursor.actions_published
                                   : 0u)
            << '\n';
        out << "valence: total=" << journal.valence.size()
            << " published=" << cursor.valence_published
            << " backlog=" << (journal.valence.size() > cursor.valence_published
                                   ? journal.valence.size() - cursor.valence_published
                                   : 0u)
            << '\n';
        const ThoughtContents thoughts = load_thoughts(thoughts_file);
        out << "thoughts: total=" << thoughts.thoughts.size()
            << " published=" << cursor.thoughts_published
            << " backlog=" << (thoughts.thoughts.size() > cursor.thoughts_published
                                   ? thoughts.thoughts.size() - cursor.thoughts_published
                                   : 0u)
            << '\n';
        return 0;
    } catch (const std::exception &error) {
        err << "statepub status failed: " << error.what() << '\n';
        return 1;
    }
}

int run_statepub_drain(std::ostream &out, std::ostream &err,
                       const RuntimeResourceStatus &resource_status,
                       const std::filesystem::path &data_dir,
                       const std::filesystem::path &ledger_file,
                       const std::filesystem::path &journal_file,
                       const std::filesystem::path &thoughts_file,
                       const std::filesystem::path &control_file,
                       bool offline) {
    require_runtime_write_headroom(resource_status);

    /* Fail-closed control gate: publication is an outbound network write
     * procedure, so the master write gate applies exactly as it does to
     * outbound actions. */
    const ControlState control = load_control_state(control_file);
    if (!control.writes_enabled) {
        err << "statepub drain refused: writes are disabled in control state\n";
        return 1;
    }

    const StateLock lock(data_dir, kStatepubLockName);

    try {
        Ledger ledger(ledger_file);

        /* Offline mode stages the exact record JSON to
         * <data>/replicate/records/ instead of the network: the same
         * cursor, the same gates, only the sink changes. A later online
         * drain resumes from the same cursor. */
        std::unique_ptr<FileWriter> file_writer;
        std::unique_ptr<WolframSession> session;
        std::unique_ptr<WolframWriter> writer;
        const auto writer_for = [&]() -> OutboundWriter & {
            if (offline) {
                if (!file_writer) {
                    file_writer = std::make_unique<FileWriter>(data_dir / "replicate" / "records");
                }
                return *file_writer;
            }
            /* Session is established lazily: a drain with no backlog
             * touches no credentials and no network. */
            if (!writer) {
                const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
                session = std::make_unique<WolframSession>(
                    service, required_env("ATPERSON_IDENTIFIER"),
                    required_env("ATPERSON_APP_PASSWORD"));
                writer = std::make_unique<WolframWriter>(*session);
            }
            return *writer;
        };

        ReplicateConfig config;
        const ReplicateReport report =
            replicate_drain(replicate_cursor_path(data_dir), journal_file, ledger,
                            thoughts_file, writer_for(), config);
        out << "observations_published=" << report.observations_published
            << " actions_published=" << report.actions_published
            << " valence_published=" << report.valence_published
            << " thoughts_published=" << report.thoughts_published
            << " withdrawal_updates=" << report.withdrawal_updates << '\n';
        if (report.network_failed) {
            err << "statepub drain stopped at a network failure: " << report.failure_detail
                << '\n';
            return 1;
        }
        if (offline) {
            out << "offline: records staged under "
                << (data_dir / "replicate" / "records").string() << '\n';
        }
        return 0;
    } catch (const std::exception &error) {
        err << "statepub drain failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace cli
} // namespace atperson
