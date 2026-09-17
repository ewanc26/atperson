#include "action.hpp"

#include <cJSON.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace atperson {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

std::string require_string(const cJSON *root, const char *name, std::string_view source) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        throw OutboundActionError(std::string(source) + ": field '" + name +
                                  "' must be a non-empty string");
    }
    return item->valuestring;
}

/* The #22 control-plane digest shape: 16 lowercase hex characters (the 64-bit
 * ledger digest). An action whose digest is not this shape can never match an
 * approved control entry, so it is rejected at parse time. */
bool is_control_digest(std::string_view digest) {
    if (digest.size() != 16u) {
        return false;
    }
    for (const char c : digest) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

void add_strong_ref(cJSON *parent, const char *name, const std::string &uri,
                    const std::string &cid) {
    cJSON *ref = cJSON_CreateObject();
    if (!ref) {
        throw OutboundActionError("failed to allocate strongRef");
    }
    cJSON_AddStringToObject(ref, "$type", "com.atproto.repo.strongRef");
    cJSON_AddStringToObject(ref, "uri", uri.c_str());
    cJSON_AddStringToObject(ref, "cid", cid.c_str());
    cJSON_AddItemToObject(parent, name, ref);
}

std::string print_json(cJSON *value, const char *context) {
    char *printed = cJSON_PrintUnformatted(value);
    if (!printed) {
        throw OutboundActionError(std::string("failed to serialise ") + context);
    }
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

} // namespace

bool outbound_action_is_reply(const OutboundAction &action) noexcept {
    return action.kind == OutboundActionKind::Reply;
}

OutboundActionProposal outbound_action_proposal(const OutboundAction &action) {
    OutboundActionProposal proposal;
    proposal.kind = action.kind;
    if (outbound_action_is_reply(action)) {
        proposal.target = action.reply_parent;
    }
    proposal.action_digest = action.digest;
    return proposal;
}

OutboundAction parse_outbound_action(std::string_view json, std::string_view source) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root) {
        throw OutboundActionError(std::string(source) + ": not valid JSON");
    }
    if (!cJSON_IsObject(root.get())) {
        throw OutboundActionError(std::string(source) + ": document must be a JSON object");
    }

    const std::string format = require_string(root.get(), "format", source);
    if (format != kOutboundActionFormat) {
        throw OutboundActionError(std::string(source) + ": unsupported format '" + format + "'");
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valueint != static_cast<int>(kOutboundActionVersion)) {
        throw OutboundActionError(std::string(source) + ": unsupported version (expected " +
                                  std::to_string(kOutboundActionVersion) + ")");
    }

    const std::string kind_name = require_string(root.get(), "kind", source);
    const auto kind = parse_outbound_kind(kind_name);
    if (!kind) {
        throw OutboundActionError(std::string(source) + ": unknown action kind '" + kind_name +
                                  "'");
    }
    if (*kind != OutboundActionKind::Post && *kind != OutboundActionKind::Reply) {
        throw OutboundActionError(std::string(source) + ": kind '" + kind_name +
                                  "' is not executable yet (only post and reply are supported)");
    }

    OutboundAction action;
    action.kind = *kind;
    action.text = require_string(root.get(), "text", source);
    action.rkey = require_string(root.get(), "rkey", source);
    action.created_at = require_string(root.get(), "created_at", source);
    action.digest = require_string(root.get(), "digest", source);
    if (!is_control_digest(action.digest)) {
        throw OutboundActionError(std::string(source) +
                                  ": 'digest' must be the 16-character lowercase hex #22 "
                                  "control digest");
    }

    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(root.get(), "reply");
    if (outbound_action_is_reply(action)) {
        if (!cJSON_IsObject(reply)) {
            throw OutboundActionError(std::string(source) + ": a reply requires a 'reply' object");
        }
        action.reply_root = require_string(reply, "root", source);
        action.reply_parent = require_string(reply, "parent", source);
    } else if (reply != nullptr) {
        throw OutboundActionError(std::string(source) +
                                  ": an original post must not carry reply context");
    }

    return action;
}

std::string serialise_outbound_action(const OutboundAction &action) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw OutboundActionError("failed to allocate action document");
    }
    cJSON_AddStringToObject(root.get(), "format", kOutboundActionFormat);
    cJSON_AddNumberToObject(root.get(), "version", static_cast<double>(kOutboundActionVersion));
    cJSON_AddStringToObject(root.get(), "kind", outbound_kind_name(action.kind));
    cJSON_AddStringToObject(root.get(), "text", action.text.c_str());
    cJSON_AddStringToObject(root.get(), "rkey", action.rkey.c_str());
    cJSON_AddStringToObject(root.get(), "created_at", action.created_at.c_str());
    cJSON_AddStringToObject(root.get(), "digest", action.digest.c_str());
    if (outbound_action_is_reply(action)) {
        cJSON *reply = cJSON_CreateObject();
        if (!reply) {
            throw OutboundActionError("failed to allocate reply context");
        }
        cJSON_AddStringToObject(reply, "root", action.reply_root.c_str());
        cJSON_AddStringToObject(reply, "parent", action.reply_parent.c_str());
        cJSON_AddItemToObject(root.get(), "reply", reply);
    }
    return print_json(root.get(), "outbound action");
}

OutboundAction load_outbound_action(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read outbound action file " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad()) {
        throw std::runtime_error("failed while reading outbound action file " + path.string());
    }
    const std::string text = buffer.str();
    return parse_outbound_action(text, path.string());
}

std::string build_outbound_record_json(const OutboundAction &action, std::string_view root_cid,
                                       std::string_view parent_cid) {
    Json record(cJSON_CreateObject());
    if (!record) {
        throw OutboundActionError("failed to allocate record body");
    }
    cJSON_AddStringToObject(record.get(), "$type", kOutboundPostCollection);
    cJSON_AddStringToObject(record.get(), "text", action.text.c_str());
    cJSON_AddStringToObject(record.get(), "createdAt", action.created_at.c_str());

    if (outbound_action_is_reply(action)) {
        if (root_cid.empty() || parent_cid.empty()) {
            throw OutboundActionError("a reply needs both root and parent CIDs");
        }
        cJSON *reply = cJSON_CreateObject();
        if (!reply) {
            throw OutboundActionError("failed to allocate reply strongRefs");
        }
        add_strong_ref(reply, "root", action.reply_root, std::string(root_cid));
        add_strong_ref(reply, "parent", action.reply_parent, std::string(parent_cid));
        cJSON_AddItemToObject(record.get(), "reply", reply);
    }

    return print_json(record.get(), "outbound record");
}

} // namespace atperson
