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
 * commands. The daemon's own account is never an implicit default, and
 * setting it here is refused downstream rather than honoured. */
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
    if (report.conflict) {
        /* A refusal, not a quiet pass. Say so and print no counts: a
         * watermark of zero would read as "seq 1 is next", which is the
         * opposite of what a refused pass means. */
        out << "remote control: refused, nothing read or written\n";
    } else {
        out << "examined " << report.examined << " record(s), applied " << report.applied
            << ", watermark " << report.watermark << '\n';
    }
    if (!report.last_op.empty()) {
        out << "last applied: " << report.last_op << '\n';
    }
    for (const RemoteRefusal &refusal : report.refusals) {
        out << "refused" << (refusal.rkey.empty() ? "" : " " + refusal.rkey) << ": "
            << refusal.reason << '\n';
    }
}

/* `emit` publishes as the operator account, so the operator can issue
 * commands from any host that holds the operator credentials. The entity's
 * own credentials are not accepted: it must not be able to author a request
 * it then obeys. */
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

    /* Rule first, credentials second: the reason the entity's own
     * credentials are not accepted is a policy, and reporting a missing
     * operator credential before stating it would bury the reason. */
    const std::string trusted = operator_did();
    if (trusted.empty()) {
        throw std::runtime_error(
            "remote control: set ATPERSON_OPERATOR_DID to the operator account you are "
            "publishing as");
    }

    /* The publisher authenticates as the OPERATOR, never as the entity.
     * The entity's own credentials are refused by construction: if they
     * could author a request, the runtime could command itself, which is
     * the one thing this channel exists to prevent. */
    const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
    WolframSession session(service, required_env("ATPERSON_OPERATOR_IDENTIFIER"),
                           required_env("ATPERSON_OPERATOR_APP_PASSWORD"));
    const std::string did = session.did();
    if (did.empty()) {
        throw std::runtime_error("remote control: session has no authenticated DID");
    }
    if (trusted != did) {
        throw std::runtime_error("remote control: this session is " + did +
                                 " but ATPERSON_OPERATOR_DID is " + trusted +
                                 "; publish from the operator's own account");
    }

    WolframWriter writer(session);
    const std::string json = serialise_control_request(request);
    const OutboundWriteResult result =
        writer.put_record(std::string(kControlCollection), control_request_rkey(seq), json);
    return "published " + result.uri + " (" + op_name + ", seq " + std::to_string(seq) +
           ") as operator " + did;
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

        /* Report the separation status even when no session is available:
         * the operator DID is in the environment, and being told "enabled"
         * when it collides with the account would be a lie. Credentials
         * are optional here, so an unset pair reports the configured DID
         * against no account and leaves the verdict to `poll`. */
        std::string account_did;
        const char *raw_account = std::getenv("ATPERSON_IDENTIFIER");
        if (raw_account != nullptr && raw_account[0] != '\0') {
            try {
                const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
                WolframSession probe(service, raw_account,
                                     required_env("ATPERSON_APP_PASSWORD"));
                account_did = probe.did();
            } catch (const std::exception &) {
                account_did.clear();
            }
        }
        const OperatorChannelStatus channel =
            check_operator_channel(account_did, trusted);
        if (channel == OperatorChannelStatus::Conflict) {
            out << "remote control: refused\n";
            out << operator_channel_denial(account_did, trusted) << '\n';
            return 1;
        }
        out << "remote control: " << (channel == OperatorChannelStatus::Disabled ? "disabled"
                                                                                 : "enabled")
            << '\n';
        if (!trusted.empty()) {
            out << "operator DID: " << trusted << '\n';
        }
        if (!account_did.empty()) {
            out << "account DID: " << account_did << " (must differ from the operator DID)\n";
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
        config.account_did = session.did();
        config.max_records = env_size("ATPERSON_REMOTE_MAX_RECORDS", 32u);
        config.max_applies = env_size("ATPERSON_REMOTE_MAX_APPLIES", 8u);
        RemoteControlChannel channel(session, config, control_state_path(), cursor_path);
        const RemotePollReport report = channel.poll();
        print_report(out, report);
        /* A collision is a misconfiguration, not a clean no-op: exit
         * non-zero so a deployment check catches it. */
        return report.conflict ? 1 : 0;
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
