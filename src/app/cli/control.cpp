#include "control.hpp"

#include "config.hpp"
#include "lock.hpp"
#include "control/state.hpp"

#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atperson {
namespace cli {
namespace {

/* Approval list bound: approvals are operator decisions, not durable
 * learning state; the cap keeps the control file bounded. */
constexpr std::size_t kMaxApprovedDigests = 64u;

[[noreturn]] void usage_error() {
    throw std::runtime_error(
        "control usage: control <status|pause|resume|writes <on|off>|dry-run <on|off>|"
        "approval <on|off>|approve <digest>|revoke <digest>|shutdown|cancel-shutdown>");
}

void save(const atperson::ControlState &state, const std::filesystem::path &control_file) {
    atperson::save_control_state(state, control_file);
}

} // namespace

int run_control(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &control_file, std::string_view sub,
                std::string_view argument) {
    auto state = atperson::load_control_state(control_file);

    if (sub == "status") {
        out << "paused: " << (state.paused ? "yes" : "no") << '\n'
            << "writes: " << (state.writes_enabled ? "enabled" : "disabled") << '\n'
            << "dry-run: " << (state.dry_run ? "on" : "off") << '\n'
            << "approval: " << (state.approval_required ? "required" : "not required")
            << '\n'
            << "approved actions: " << state.approved_digests.size() << '\n';
        if (state.last_sync_at) {
            out << "last successful sync: " << *state.last_sync_at << '\n';
        }
        if (state.shutdown_requested_at) {
            out << "shutdown requested: " << *state.shutdown_requested_at << '\n';
        }
        return 0;
    }

    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);

    if (sub == "pause") {
        state.paused = true;
        save(state, control_file);
        out << "paused; ingest and sync will refuse until resumed\n";
        return 0;
    }

    if (sub == "resume") {
        state.paused = false;
        save(state, control_file);
        out << "resumed\n";
        return 0;
    }

    if (sub == "writes") {
        if (argument != "on" && argument != "off") {
            usage_error();
        }
        state.writes_enabled = argument == "on";
        save(state, control_file);
        out << "network writes " << (state.writes_enabled ? "enabled" : "disabled")
            << (state.writes_enabled && state.dry_run
                    ? " (dry-run still refuses execution)"
                    : "")
            << '\n';
        return 0;
    }

    if (sub == "dry-run") {
        if (argument != "on" && argument != "off") {
            usage_error();
        }
        state.dry_run = argument == "on";
        save(state, control_file);
        out << "dry-run " << (state.dry_run ? "on" : "off") << '\n';
        return 0;
    }

    if (sub == "approval") {
        if (argument != "on" && argument != "off") {
            usage_error();
        }
        state.approval_required = argument == "on";
        save(state, control_file);
        out << "manual approval " << (state.approval_required ? "required" : "not required")
            << '\n';
        return 0;
    }

    if (sub == "approve") {
        if (argument.empty()) {
            usage_error();
        }
        if (!atperson::is_digest_approved(state, argument)) {
            if (state.approved_digests.size() >= kMaxApprovedDigests) {
                throw std::runtime_error("approval list is full; revoke an action first");
            }
            state.approved_digests.insert(state.approved_digests.begin(),
                                           std::string(argument));
        }
        save(state, control_file);
        out << "approved " << argument << '\n';
        return 0;
    }

    if (sub == "revoke") {
        if (argument.empty()) {
            usage_error();
        }
        const auto before = state.approved_digests.size();
        std::erase_if(state.approved_digests,
                      [&argument](const std::string &digest) { return digest == argument; });
        if (state.approved_digests.size() == before) {
            out << "no approval for " << argument << '\n';
            return 0;
        }
        save(state, control_file);
        out << "revoked " << argument << '\n';
        return 0;
    }

    if (sub == "shutdown") {
        state.shutdown_requested_at = atperson::control_now_rfc3339();
        save(state, control_file);
        out << "shutdown requested; a running daemon will snapshot and exit\n";
        return 0;
    }

    if (sub == "cancel-shutdown") {
        state.shutdown_requested_at.reset();
        save(state, control_file);
        out << "shutdown request cancelled\n";
        return 0;
    }

    usage_error();
}

} // namespace cli
} // namespace atperson
