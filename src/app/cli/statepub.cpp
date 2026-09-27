#include "statepub.hpp"

#include "../atproto/session.hpp"
#include "../atproto/writer.hpp"
#include "../journal/store.hpp"
#include "../replicate/publication.hpp"
#include "../thought/store.hpp"
#include "config.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>

namespace atperson {
namespace cli {

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
            Ledger ledger(ledger_file);
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
    /* One implementation of the gates, the lock and the drain, shared with
     * the daemon's automatic pass: a refusal here is the same refusal
     * there, and there is no second publication path to drift. */
    const PublicationPaths paths{
        data_dir,
        replicate_cursor_path(data_dir),
        replicate_records_dir(data_dir),
        journal_file,
        thoughts_file,
        control_file};

    try {
        Ledger ledger(ledger_file);

        /* The session is established lazily inside the pass: a drain with no
         * backlog touches no credentials and no network. */
        std::unique_ptr<WolframSession> session;
        std::unique_ptr<WolframWriter> writer;
        const OnlineWriterFactory online_writer = [&]() -> OutboundWriter & {
            if (!writer) {
                const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
                session = std::make_unique<WolframSession>(
                    service, required_env("ATPERSON_IDENTIFIER"),
                    required_env("ATPERSON_APP_PASSWORD"));
                writer = std::make_unique<WolframWriter>(*session);
            }
            return *writer;
        };

        const PublicationReport report =
            run_publication_pass(paths, publication_config_from_environment(), resource_status,
                                 ledger, offline, online_writer);
        if (report.refused) {
            err << "statepub drain refused: " << report.refusal << '\n';
            return 1;
        }
        out << "observations_published=" << report.drain.observations_published
            << " actions_published=" << report.drain.actions_published
            << " valence_published=" << report.drain.valence_published
            << " thoughts_published=" << report.drain.thoughts_published
            << " withdrawal_updates=" << report.drain.withdrawal_updates << '\n';
        if (report.drain.network_failed) {
            err << "statepub drain stopped at a network failure: "
                << report.drain.failure_detail << '\n';
            return 1;
        }
        if (offline) {
            out << "offline: records staged under " << replicate_records_dir(data_dir).string()
                << '\n';
        }
        return 0;
    } catch (const std::exception &error) {
        err << "statepub drain failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace cli
} // namespace atperson
