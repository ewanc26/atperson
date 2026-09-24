#include "reconstruct.hpp"

#include "../atproto/record_source.hpp"
#include "../atproto/session.hpp"
#include "../replicate/reconstruct.hpp"
#include "config.hpp"

#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {

int run_reconstruct(std::ostream &out, std::ostream &err, const std::filesystem::path &into_dir) {
    try {
        if (into_dir.empty() || !std::filesystem::exists(into_dir) ||
            !std::filesystem::is_directory(into_dir)) {
            err << "reconstruct: --into must be an existing, empty directory\n";
            return 2;
        }
        const std::filesystem::path ledger_file = into_dir / "ledger.bin";
        const std::filesystem::path journal_file = into_dir / "action-journal.jsonl";
        const std::filesystem::path thoughts_file = into_dir / "thoughts.jsonl";

        const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
        WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                               required_env("ATPERSON_APP_PASSWORD"));
        WolframRecordSource source(session);

        const ReconstructReport report =
            reconstruct_state(source, ledger_file, journal_file, thoughts_file);
        out << "observations_replayed=" << report.observations_replayed
            << " observations_skipped_withdrawn=" << report.observations_skipped_withdrawn
            << " observations_failed=" << report.observations_failed
            << " actions_replayed=" << report.actions_replayed
            << " valence_replayed=" << report.valence_replayed
            << " thoughts_replayed=" << report.thoughts_replayed
            << " records_corrupt=" << report.records_corrupt << '\n';
        for (const std::string &failure : report.failures) {
            err << "reconstruct: " << failure << '\n';
        }
        return report.failures.empty() ? 0 : 1;
    } catch (const std::exception &error) {
        err << "reconstruct failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace cli
} // namespace atperson
