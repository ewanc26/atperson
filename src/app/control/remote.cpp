#include "remote.hpp"

#include <cJSON.h>

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <unistd.h>

namespace atperson {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

constexpr std::string_view kCursorFormat = "atperson-control-cursor";
constexpr std::uint32_t kCursorVersion = 1u;

/* Bound the approval list the same way the local CLI does, so a remote
 * approve cannot grow the durable file past what a local approve allows. */
constexpr std::size_t kMaxApprovedDigests = 64u;

[[noreturn]] void invalid(const std::string &message) {
    throw ControlRemoteError("remote control: " + message);
}

std::string require_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
        invalid(std::string("missing or invalid string field '") + key + "'");
    }
    return value->valuestring;
}

std::optional<std::string> optional_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value || cJSON_IsNull(value)) {
        return std::nullopt;
    }
    if (!cJSON_IsString(value) || !value->valuestring) {
        invalid(std::string("field '") + key + "' must be a string or null");
    }
    return std::string(value->valuestring);
}

/* `seq` is a decimal string so the full 64 bits survive a JSON round-trip
 * (doubles carry 53). A bare number is accepted for hand-written records
 * but is checked for integrality. */
std::uint64_t require_u64_field(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring) {
        const std::string text(value->valuestring);
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
            invalid(std::string("field '") + key + "' is not a decimal string");
        }
        try {
            return std::stoull(text);
        } catch (const std::exception &) {
            invalid(std::string("field '") + key + "' overflows u64");
        }
    }
    if (!cJSON_IsNumber(value) || value->valuedouble < 0.0) {
        invalid(std::string("missing or invalid field '") + key + "'");
    }
    const double raw = value->valuedouble;
    const double truncated = static_cast<double>(static_cast<std::uint64_t>(raw));
    if (raw != truncated) {
        invalid(std::string("field '") + key + "' must be a whole number");
    }
    return static_cast<std::uint64_t>(raw);
}

std::string print_json(const cJSON *value) {
    char *raw = cJSON_PrintUnformatted(value);
    if (!raw) {
        throw std::runtime_error("remote control: JSON printing failed");
    }
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

std::string serialise_cursor(const RemoteControlCursor &cursor) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("remote control: JSON allocation failed");
    }
    cJSON_AddStringToObject(root.get(), "format", kCursorFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kCursorVersion);
    cJSON_AddStringToObject(root.get(), "last_seq", std::to_string(cursor.last_seq).c_str());
    return print_json(root.get());
}

} // namespace

std::string_view control_op_name(ControlOp op) {
    switch (op) {
    case ControlOp::Pause:
        return "pause";
    case ControlOp::Resume:
        return "resume";
    case ControlOp::WritesOn:
        return "writes-on";
    case ControlOp::WritesOff:
        return "writes-off";
    case ControlOp::DryRunOn:
        return "dry-run-on";
    case ControlOp::DryRunOff:
        return "dry-run-off";
    case ControlOp::OfflineOn:
        return "offline-on";
    case ControlOp::OfflineOff:
        return "offline-off";
    case ControlOp::ApprovalOn:
        return "approval-on";
    case ControlOp::ApprovalOff:
        return "approval-off";
    case ControlOp::Approve:
        return "approve";
    case ControlOp::Revoke:
        return "revoke";
    case ControlOp::Shutdown:
        return "shutdown";
    case ControlOp::CancelShutdown:
        return "cancel-shutdown";
    }
    invalid("unknown control op");
}

std::optional<ControlOp> control_op_from_name(std::string_view name) {
    /* Ordered to match the local `atperson control` subcommands. */
    static constexpr ControlOp kAll[] = {
        ControlOp::Pause,       ControlOp::Resume,         ControlOp::WritesOn,
        ControlOp::WritesOff,   ControlOp::DryRunOn,       ControlOp::DryRunOff,
        ControlOp::OfflineOn,   ControlOp::OfflineOff,     ControlOp::ApprovalOn,
        ControlOp::ApprovalOff, ControlOp::Approve,        ControlOp::Revoke,
        ControlOp::Shutdown,    ControlOp::CancelShutdown,
    };
    for (const ControlOp candidate : kAll) {
        if (control_op_name(candidate) == name) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool control_op_takes_argument(ControlOp op) noexcept {
    return op == ControlOp::Approve || op == ControlOp::Revoke;
}

OperatorChannelStatus check_operator_channel(std::string_view account_did,
                                             std::string_view operator_did) {
    if (operator_did.empty()) {
        return OperatorChannelStatus::Disabled;
    }
    /* The runtime must not be able to command itself. Same DID means the
     * entity's own credentials could author every request it then obeys,
     * so the channel would grant the governed party the authority that is
     * supposed to sit outside it. */
    if (!account_did.empty() && account_did == operator_did) {
        return OperatorChannelStatus::Conflict;
    }
    return OperatorChannelStatus::Ready;
}

std::string operator_channel_denial(std::string_view account_did,
                                    std::string_view operator_did) {
    switch (check_operator_channel(account_did, operator_did)) {
    case OperatorChannelStatus::Disabled:
        return "remote control is disabled: set ATPERSON_OPERATOR_DID to a DID that is "
               "not this entity's account";
    case OperatorChannelStatus::Ready:
        return {};
    case OperatorChannelStatus::Conflict:
        return "remote control refused: ATPERSON_OPERATOR_DID is " + std::string(operator_did) +
               ", which is also this entity's own account DID. The operator must be a "
               "separate account, or the entity could command itself; set "
               "ATPERSON_OPERATOR_DID to a different DID or unset it to disable the "
               "channel";
    }
    return "remote control is unavailable";
}

std::string serialise_control_request(const ControlRequest &request) {
    if (control_op_takes_argument(request.op)) {
        if (request.arg.empty()) {
            invalid(std::string("op '") + std::string(control_op_name(request.op)) +
                    "' requires a digest argument");
        }
    } else if (!request.arg.empty()) {
        invalid(std::string("op '") + std::string(control_op_name(request.op)) +
                "' does not take an argument");
    }

    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("remote control: JSON allocation failed");
    }
    cJSON_AddStringToObject(root.get(), "format", kControlRequestFormat.data());
    cJSON_AddNumberToObject(root.get(), "version", kControlRequestVersion);
    cJSON_AddStringToObject(root.get(), "type", kControlRequestType.data());
    cJSON_AddStringToObject(root.get(), "seq", std::to_string(request.seq).c_str());
    cJSON_AddStringToObject(root.get(), "op", control_op_name(request.op).data());
    if (control_op_takes_argument(request.op)) {
        cJSON_AddStringToObject(root.get(), "arg", request.arg.c_str());
    }
    if (request.at) {
        cJSON_AddStringToObject(root.get(), "at", request.at->c_str());
    } else {
        cJSON_AddNullToObject(root.get(), "at");
    }
    return print_json(root.get());
}

ControlRequest parse_control_request(std::string_view json) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        invalid("not a JSON object");
    }
    if (require_string(root.get(), "format") != kControlRequestFormat) {
        invalid("unknown request format");
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != kControlRequestVersion) {
        invalid("unsupported request version");
    }
    /* The record type is informational for a reader that already fetched
     * by collection, but a mismatch means the document is not the shape
     * this channel understands, so refuse rather than guess. */
    if (require_string(root.get(), "type") != kControlRequestType) {
        invalid("unexpected record type");
    }

    ControlRequest request;
    request.seq = require_u64_field(root.get(), "seq");
    const std::string op_name = require_string(root.get(), "op");
    const std::optional<ControlOp> op = control_op_from_name(op_name);
    if (!op.has_value()) {
        invalid("unknown op '" + op_name + "'");
    }
    request.op = *op;
    request.arg = optional_string(root.get(), "arg").value_or(std::string{});
    request.at = optional_string(root.get(), "at");

    if (control_op_takes_argument(request.op)) {
        if (request.arg.empty()) {
            invalid(std::string("op '") + op_name + "' requires a digest argument");
        }
    } else if (!request.arg.empty()) {
        invalid(std::string("op '") + op_name + "' does not take an argument");
    }
    return request;
}

std::string control_request_rkey(std::uint64_t seq) {
    /* TID alphabet in ascending ASCII order ('2' < '7' < 'a' < 'z'), so a
     * fixed-width encoding sorts numerically. */
    static constexpr char alphabet[] = "234567abcdefghijklmnopqrstuvwxyz";
    std::string rkey(13u, '2');
    for (int i = 12; i >= 0; --i) {
        rkey[static_cast<std::size_t>(i)] = alphabet[seq & 0x1Fu];
        seq >>= 5;
    }
    return rkey;
}

RemoteApplyReport apply_control_request(ControlState &state, std::uint64_t &watermark,
                                        const ControlRequest &request,
                                        std::string_view operator_did,
                                        std::string_view request_did) {
    RemoteApplyReport report;
    report.seq = request.seq;
    report.op = std::string(control_op_name(request.op));

    /* 1. Provenance. An unsigned record is not a command. */
    if (operator_did.empty()) {
        report.reason = "no operator DID configured; remote control is disabled";
        return report;
    }
    if (request_did != operator_did) {
        report.reason = "record author is not the configured operator";
        return report;
    }

    /* 2. Freshness. Strictly the next sequence: a gap means a command was
     * lost, and a repeat means a replay. Either way, refuse rather than
     * apply a command that is not the one the operator sequenced. */
    if (request.seq != watermark + 1u) {
        report.reason = request.seq <= watermark ? "sequence already applied or replayed"
                                                 : "sequence gap; an earlier command is missing";
        return report;
    }

    /* 3. Shape, re-checked here so a caller that built a request in
     * memory cannot skip the argument rule the parser enforces. */
    if (control_op_takes_argument(request.op)) {
        if (request.arg.empty()) {
            report.reason = "op requires a digest argument";
            return report;
        }
    } else if (!request.arg.empty()) {
        report.reason = "op does not take an argument";
        return report;
    }

    /* The approval list is bounded exactly as the local CLI bounds it, so
     * a remote approve cannot outgrow the durable file. */
    if (request.op == ControlOp::Approve && !is_digest_approved(state, request.arg) &&
        state.approved_digests.size() >= kMaxApprovedDigests) {
        report.reason = "approval list is full; revoke an action first";
        return report;
    }

    /* Past every check: mutate. */
    switch (request.op) {
    case ControlOp::Pause:
        state.paused = true;
        break;
    case ControlOp::Resume:
        state.paused = false;
        break;
    case ControlOp::WritesOn:
        state.writes_enabled = true;
        break;
    case ControlOp::WritesOff:
        state.writes_enabled = false;
        break;
    case ControlOp::DryRunOn:
        state.dry_run = true;
        break;
    case ControlOp::DryRunOff:
        state.dry_run = false;
        break;
    case ControlOp::OfflineOn:
        state.offline_mode = true;
        break;
    case ControlOp::OfflineOff:
        state.offline_mode = false;
        break;
    case ControlOp::ApprovalOn:
        state.approval_required = true;
        break;
    case ControlOp::ApprovalOff:
        state.approval_required = false;
        break;
    case ControlOp::Approve:
        if (!is_digest_approved(state, request.arg)) {
            state.approved_digests.insert(state.approved_digests.begin(), request.arg);
        }
        break;
    case ControlOp::Revoke:
        std::erase_if(state.approved_digests,
                      [&request](const std::string &digest) { return digest == request.arg; });
        break;
    case ControlOp::Shutdown:
        state.shutdown_requested_at = control_now_rfc3339();
        break;
    case ControlOp::CancelShutdown:
        state.shutdown_requested_at.reset();
        break;
    }

    watermark = request.seq;
    report.applied = true;
    report.reason = "applied";
    return report;
}

std::optional<std::string> aturi_authority(std::string_view uri) {
    static constexpr std::string_view kPrefix = "at://";
    if (uri.size() <= kPrefix.size() || uri.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    const std::string_view rest = uri.substr(kPrefix.size());
    const std::string_view::size_type slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0u) {
        return std::nullopt;
    }
    const std::string_view authority = rest.substr(0, slash);
    /* A DID always starts with `did:`. Rejecting anything else keeps a
     * handle or a bare collection from ever matching an operator DID. */
    if (authority.substr(0, 4) != "did:") {
        return std::nullopt;
    }
    return std::string(authority);
}

bool is_operator_control_uri(std::string_view uri, std::string_view did,
                             std::string_view collection) {
    if (did.empty()) {
        return false;
    }
    const std::optional<std::string> authority = aturi_authority(uri);
    if (!authority.has_value() || *authority != did) {
        return false;
    }
    /* Require the collection as a path segment so a control record cannot
     * be smuggled through a different collection on the same repo. */
    const std::string expected = "/" + std::string(collection) + "/";
    return uri.find(expected) != std::string_view::npos;
}

RemoteControlCursor load_remote_control_cursor(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("remote control: could not read " + path.string());
    }
    std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json root(cJSON_ParseWithLength(payload.data(), payload.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        throw std::runtime_error("remote control: cursor is not a JSON object");
    }
    if (require_string(root.get(), "format") != kCursorFormat) {
        invalid("unknown cursor format");
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != kCursorVersion) {
        invalid("unsupported cursor version");
    }
    RemoteControlCursor cursor;
    cursor.version = kCursorVersion;
    cursor.last_seq = require_u64_field(root.get(), "last_seq");
    return cursor;
}

void save_remote_control_cursor(const RemoteControlCursor &cursor,
                                const std::filesystem::path &path) {
    const std::string payload = serialise_cursor(cursor);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code parent_ec;
        std::filesystem::create_directories(parent, parent_ec);
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("remote control: could not create " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("remote control: could not write " + tmp.string());
        }
    }

    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("remote control: could not commit " + path.string() + ": " +
                                 rename_ec.message());
    }
    /* The cursor is the replay guard, so its commit has to be durable
     * before a later cycle trusts it. */
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const int dir = ::open(parent.c_str(), O_RDONLY);
        if (dir >= 0) {
            (void)::fsync(dir);
            ::close(dir);
        }
    }
}

} // namespace atperson
