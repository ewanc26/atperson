#include "control_remote.hpp"

#include "atproto/session.hpp"
#include "atproto/writer.hpp"
#include "config.hpp"
#include "control/remote.hpp"
#include "control/remote_poll.hpp"

#include <cstdlib>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {
namespace {

/* The trusted authority. Empty means the channel is inert: no DID, no
 * commands. The daemon's own account is never an implicit default —
 * self-authorised control is a deliberate deployment choice. */
std::string operator_did() {
    const char *raw = std::getenv("ATPERSON_OPERATOR_DID");
    return raw != nullptr && raw[0] != '\0' ? std::string(raw) : std::string{};
}

std::size_t env_size(const char *name, std::size_t fallback) {
    const char *raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }
    try {
        const unsigned long long parsed = std::stoull(raw);
        return parsed > 0u ? static_cast<std::size_t>(parsed) : fallback;
    } catch (const std::exception &) {
        return fallback;
    }
}

[[noreturn]] void usage_error() {
    throw std::runtime_error(
        "control remote usage: control remote <status|poll|emit <op> [digest]>");
}

void print_report(std::ostream &out, const RemotePollReport &report) {
    out << "examined " << report.examined << " record(s), applied " << report.applied
        << ", watermark " << report.watermark << '\n';
    if (!report.last_op.empty()) {
        out << "last applied: " << report.last_op << '\n';
    }
    for (const RemoteRefusal &refusal : report.refusals) {
        out << "refused" << (refusal.rkey.empty() ? "" : " " + refusal.rkey) << ": "
            << refusal.reason << '\n';
    }
}

/* `emit` writes the request as a record under a TID rkey, so the operator
 * can issue commands from any host that has these credentials. */
std::string emit(const std::string &op_name, const std::string &arg) {
    const std::optional<ControlOp> op = control_op_from_name(op_name);
    if (!op.has_value()) {
        throw ControlRemoteError("remote control: unknown op '" + op_name + "'");
    }

    /* The next sequence is the watermark plus one: the same rule the
     * poller enforces, so an operator cannot accidentally publish a
     * request the daemon will refuse as a gap. */
    const auto cursor_path = remote_control_cursor_path();
    std::uint64_t seq = 0u;
    try {
        seq = load_remote_control_cursor(cursor_path).last_seq + 1u;
    } catch (const std::exception &) {
        /* An unreadable local cursor is not a reason to refuse an emit:
         * the operator may be publishing from a different host, where the
         * real watermark lives on the daemon. Fall back to a timestamp,
         * which is monotonic and far ahead of any hand-issued sequence. */
        seq = static_cast<std::uint64_t>(std::time(nullptr));
    }

    ControlRequest request;
    request.seq = seq;
    request.op = *op;
    request.arg = arg;
    request.at = control_now_rfc3339();

    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                           required_env("ATPERSON_APP_PASSWORD"));
    const std::string did = session.did();
    if (did.empty()) {
        throw std::runtime_error("remote control: session has no authenticated DID");
    }
    /* Refuse to emit into a repo the daemon does not trust: a request
     * published anywhere else is inert by design, so failing loudly here
     * beats a silent no-op. */
    const std::string trusted = operator_did();
    if (!trusted.empty() && trusted != did) {
        throw std::runtime_error("remote control: this session is " + did +
                                 " but ATPERSON_OPERATOR_DID is " + trusted +
                                 "; publish from the operator's own account");
    }

    WolframWriter writer(session);
    const std::string json = serialise_control_request(request);
    const OutboundWriteResult result =
        writer.put_record(std::string(kControlCollection), control_request_rkey(seq), json);
    return "published " + result.uri + " (" + op_name + ", seq " + std::to_string(seq) + ")";
}

} // namespace

int run_control_remote(std::ostream &out, const RuntimeResourceStatus &resource_status,
                       std::string_view sub, std::string_view argument) {
    const std::string trusted = operator_did();
    const auto cursor_path = remote_control_cursor_path();

    if (sub == "status") {
        std::uint64_t last_seq = 0u;
        std::string cursor_error;
        try {
            last_seq = load_remote_control_cursor(cursor_path).last_seq;
        } catch (const std::exception &error) {
            cursor_error = error.what();
        }
        out << "remote control: " << (trusted.empty() ? "disabled" : "enabled") << '\n';
        if (!trusted.empty()) {
            out << "operator DID: " << trusted << '\n';
        }
        if (cursor_error.empty()) {
            out << "last applied sequence: " << last_seq << '\n';
            out << "next admissible sequence: " << (last_seq + 1u) << '\n';
        } else {
            /* Never report zero here: a zero would read as "accept
             * anything", which is exactly what a broken cursor must not
             * imply. */
            out << "cursor unreadable: " << cursor_error << '\n';
        }
        return cursor_error.empty() ? 0 : 1;
    }

    if (sub == "poll") {
        require_runtime_write_headroom(resource_status);
        const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
        WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                               required_env("ATPERSON_APP_PASSWORD"));
        RemotePollConfig config;
        config.operator_did = trusted;
        config.max_records = env_size("ATPERSON_REMOTE_MAX_RECORDS", 32u);
        config.max_applies = env_size("ATPERSON_REMOTE_MAX_APPLIES", 8u);
        RemoteControlChannel channel(session, config, control_state_path(), cursor_path);
        print_report(out, channel.poll());
        return 0;
    }

    if (sub == "emit") {
        const std::vector<std::string_view> words = [&] {
            std::vector<std::string_view> parts;
            std::string_view rest = argument;
            while (!rest.empty()) {
                const std::string_view::size_type space = rest.find(' ');
                if (space == std::string_view::npos) {
                    parts.push_back(rest);
                    break;
                }
                parts.push_back(rest.substr(0, space));
                rest.remove_prefix(space + 1u);
            }
            return parts;
        }();
        if (words.empty()) {
            usage_error();
        }
        const std::string arg = words.size() > 1u ? std::string(words[1]) : std::string{};
        out << emit(std::string(words[0]), arg) << '\n';
        return 0;
    }

    usage_error();
}

} // namespace cli
} // namespace atperson
