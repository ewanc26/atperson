// CLI ingestion-cursor command: cursor [status|reset].
//
// Implementation of the contract in cursor.hpp. The body is moved
// byte-faithfully from the original single-file dispatch in src/app/main.cpp;
// behaviour, ordering and output text are unchanged.

#include "cursor.hpp"

#include "atproto_client.hpp"
#include "config.hpp"
#include "ingestion_state.hpp"
#include "state_lock.hpp"

#include <ostream>
#include <string>

namespace atperson {
namespace cli {

int run_cursor(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir,
               const std::filesystem::path &state_file, std::string_view sub) {
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    atperson::AtprotoClient client(service, required_env("ATPERSON_IDENTIFIER"),
                                    required_env("ATPERSON_APP_PASSWORD"));
    auto state =
        atperson::load_ingestion_state(state_file, service, client.account_did());
    if (sub == "reset") {
        atperson::require_runtime_write_headroom(resource_status);
        const atperson::StateLock writer_lock(data_dir);
        atperson::reset_ingestion_state(state);
        state.checkpoint.generation++;
        atperson::save_ingestion_state(state, state_file);
        out << "ingestion cursor reset; next sync starts at the timeline head\n";
        return 0;
    }
    out << "source: " << state.source.kind << " " << state.source.service << " "
        << state.source.account_did << " " << state.source.endpoint << '\n'
        << "catch-up: " << (state.catchup.active ? "active" : "inactive") << '\n'
        << "checkpoint: generation " << state.checkpoint.generation << ", pages "
        << state.checkpoint.pages_completed << ", observations "
        << state.checkpoint.observations_seen << '\n';
    return 0;
}

} // namespace cli
} // namespace atperson
