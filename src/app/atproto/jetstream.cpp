/* Jetstream commit -> SyncObservation translation (#60).
 *
 * Jetstream delivers a flattened JSON commit (not the firehose CAR shape):
 * the record is already a parsed JSON object inside the commit envelope, so
 * translation is a pure JSON walk over `record` plus the commit's
 * collection/rkey/did/time. This module owns the semantics; the network
 * client owns transport and owns the cJSON tree.
 *
 * The contract mirrors `atproto/extract.hpp`: it consumes one raw Jetstream
 * JSON message (the same bytes `wf_jetstream_event_parse` would consume) and
 * produces a SyncObservation. No HTTP, no Wolfram, no filesystem: this is the
 * offline-testable half, and the network client is responsible for calling
 * `wf_jetstream_event_parse` first and passing the resulting JSON here only
 * when the event kind is a commit.
 *
 * Malformed context is handled gracefully: a reply whose parent record is
 * missing or malformed yields empty identifiers rather than a dropped item,
 * and a quote embed that fails to resolve yields no quote URI.
 *
 * The source is unauthenticated, so there is no account DID to record here;
 * the ingestion-state `source.account_did` is empty for the Jetstream feed
 * and the CLI owns that identity, not the extractor.
 */

#include "jetstream.hpp"

#include "engine.hpp"
#include "policy.hpp"

#include <cJSON.h>
#include <cstring>
#include <string>
#include <string_view>

namespace atperson {

namespace {

constexpr std::string_view kPostCollection = "app.bsky.feed.post";

std::string string_field(const cJSON *object, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (value == nullptr || !cJSON_IsString(value) || value->valuestring == nullptr) {
        return std::string();
    }
    return value->valuestring;
}

/* The reply block carries strongRef objects ({uri, cid}) to root and parent;
 * only the URIs interest the engine. */
void parse_reply_context(const cJSON *record, std::string &reply_root,
                         std::string &reply_parent) {
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(record, "reply");
    if (reply == nullptr || !cJSON_IsObject(reply)) {
        return;
    }
    const cJSON *root = cJSON_GetObjectItemCaseSensitive(reply, "root");
    if (root != nullptr && cJSON_IsObject(root)) {
        reply_root = string_field(root, "uri");
    }
    const cJSON *parent = cJSON_GetObjectItemCaseSensitive(reply, "parent");
    if (parent != nullptr && cJSON_IsObject(parent)) {
        reply_parent = string_field(parent, "uri");
    }
}

/* A quote embed carries a strongRef to the quoted record. */
std::string parse_quote_context(const cJSON *record) {
    const cJSON *embed = cJSON_GetObjectItemCaseSensitive(record, "embed");
    if (embed == nullptr || !cJSON_IsObject(embed)) {
        return std::string();
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(embed, "$type");
    if (type == nullptr || !cJSON_IsString(type) || type->valuestring == nullptr ||
        std::strcmp(type->valuestring, "app.bsky.embed.record") != 0) {
        return std::string();
    }
    const cJSON *quoted = cJSON_GetObjectItemCaseSensitive(embed, "record");
    if (quoted == nullptr || !cJSON_IsObject(quoted)) {
        return std::string();
    }
    return string_field(quoted, "uri");
}

} // namespace

bool extract_jetstream_commit(
    const char *json, size_t json_len, std::string_view account_did,
    SyncObservation &out) {
    if (json == nullptr || json_len == 0u) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == nullptr || !cJSON_IsObject(root)) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        return false;
    }

    const cJSON *commit = cJSON_GetObjectItemCaseSensitive(root, "commit");
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(commit, "operation");
    const cJSON *collection = cJSON_GetObjectItemCaseSensitive(commit, "collection");
    const cJSON *rkey = cJSON_GetObjectItemCaseSensitive(commit, "rkey");
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(commit, "record");
    const cJSON *did = cJSON_GetObjectItemCaseSensitive(root, "did");
    const cJSON *time = cJSON_GetObjectItemCaseSensitive(root, "time");

    const bool ok = cJSON_IsObject(commit) && cJSON_IsString(operation) &&
                    operation->valuestring != nullptr &&
                    (std::strcmp(operation->valuestring, "create") == 0 ||
                     std::strcmp(operation->valuestring, "update") == 0) &&
                    cJSON_IsString(collection) && collection->valuestring != nullptr &&
                    std::string_view(collection->valuestring) == kPostCollection &&
                    cJSON_IsString(rkey) && rkey->valuestring != nullptr &&
                    cJSON_IsObject(record) && cJSON_IsString(did) &&
                    did->valuestring != nullptr && did->valuestring[0] != '\0';

    if (!ok) {
        cJSON_Delete(root);
        return false;
    }

    const std::string text_value = string_field(record, "text");
    std::string reply_root;
    std::string reply_parent;
    parse_reply_context(record, reply_root, reply_parent);
    const std::string quote_uri = parse_quote_context(record);

    PolicyPost policy_post;
    policy_post.author_did = did->valuestring;
    policy_post.record_type = string_field(record, "$type");
    policy_post.text = text_value;
    policy_post.is_reply = !reply_parent.empty();
    policy_post.is_quote = !quote_uri.empty();
    const cJSON *embed = cJSON_GetObjectItemCaseSensitive(record, "embed");
    if (embed != nullptr && cJSON_IsObject(embed)) {
        policy_post.embed_type = string_field(embed, "$type");
    }
    policy_post.embed_has_text_fallback = false;

    const PolicyDecision decision = evaluate_post(account_did, policy_post);

    out = SyncObservation{};
    out.text = decision.eligible ? text_value : std::string{};
    out.source_uri = std::string("at://") + did->valuestring + "/app.bsky.feed.post/" +
                     rkey->valuestring;
    out.author_did = did->valuestring;
    /* Prefer the record's own created-at (matching the timeline extractor);
     * the commit envelope timestamp is the fallback for malformed records. */
    const std::string record_created = string_field(record, "createdAt");
    if (!record_created.empty()) {
        out.created_at = record_created;
    } else if (cJSON_IsString(time) && time->valuestring != nullptr) {
        out.created_at = time->valuestring;
    }
    out.context.reply_root_uri = reply_root;
    out.context.reply_parent_uri = reply_parent;
    out.context.quote_uri = quote_uri;
    out.policy_reason = decision.reason;

    cJSON_Delete(root);
    return true;
}

} // namespace atperson