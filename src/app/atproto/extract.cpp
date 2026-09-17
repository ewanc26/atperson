#include "extract.hpp"

#include "policy.hpp"

#include <string>
#include <string_view>

namespace atperson {

namespace {

const char *json_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : nullptr;
}

bool json_bool(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

/* Read a `reply`-shaped reference: {uri, cid, author}. Only the URI is
 * stable provenance worth keeping — CIDs change with record edits and the
 * author DID is derivable from the URI. Absent or malformed references
 * read as empty; the observation survives with partial context. */
std::string ref_uri(cJSON *object, const char *key) {
    cJSON *ref = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsObject(ref)) {
        return {};
    }
    const char *uri = json_string(ref, "uri");
    return uri ? std::string(uri) : std::string{};
}

} // namespace

bool extract_feed_item(cJSON *item, std::string_view account_did,
                       SyncObservation &out) {
    if (!cJSON_IsObject(item)) {
        return false;
    }
    cJSON *post = cJSON_GetObjectItemCaseSensitive(item, "post");
    if (!cJSON_IsObject(post)) {
        return false;
    }
    const char *uri = json_string(post, "uri");
    if (!uri) {
        return false;
    }
    cJSON *record = cJSON_GetObjectItemCaseSensitive(post, "record");
    if (!cJSON_IsObject(record)) {
        return false;
    }

    out = SyncObservation{};
    out.source_uri = uri;
    PolicyPost policy_post;

    /* Reply context: the feedViewPost `reply` member carries strongRefs to
     * the thread root and direct parent. A deleted parent still yields the
     * identifiers the author's record claims — that is exactly what a
     * planner needs, and what an audit wants to see. */
    cJSON *reply = cJSON_GetObjectItemCaseSensitive(item, "reply");
    if (cJSON_IsObject(reply)) {
        out.context.reply_root_uri = ref_uri(reply, "root");
        out.context.reply_parent_uri = ref_uri(reply, "parent");
    }

    /* Quote context: app.bsky.embed.record embeds the quoted post. The
     * quoted record's text is NOT merged into the learnable text — the
     * policy tags quotes separately and the quote URI rides in context. */
    cJSON *embed = cJSON_GetObjectItemCaseSensitive(record, "embed");
    if (cJSON_IsObject(embed)) {
        const char *embed_type = json_string(embed, "$type");
        policy_post.embed_type = embed_type ? embed_type : "";
        if (policy_post.embed_type == "app.bsky.embed.record") {
            out.context.quote_uri = ref_uri(embed, "record");
            policy_post.is_quote = true;
        }
        /* Quote embeds are NOT a text fallback (issue #24): the quoted
         * record's text is never learnable input. Only the quoting
         * author's own text counts, and a quote with no own text is
         * honestly skipped as empty. */
        policy_post.embed_has_text_fallback = false;
    }

    /* Policy fields, extracted (never decided) here. */
    const char *record_type = json_string(record, "$type");
    policy_post.record_type = record_type ? record_type : "";
    cJSON *author = cJSON_GetObjectItemCaseSensitive(post, "author");
    const char *did = cJSON_IsObject(author) ? json_string(author, "did") : nullptr;
    policy_post.author_did = did ? did : "";
    const char *text = json_string(record, "text");
    policy_post.text = text ? std::string_view(text) : std::string_view{};
    policy_post.is_reply = !out.context.reply_parent_uri.empty();

    cJSON *viewer = cJSON_GetObjectItemCaseSensitive(post, "viewer");
    if (cJSON_IsObject(viewer)) {
        const char *blocking = json_string(viewer, "blocking");
        policy_post.viewer_blocked = blocking && blocking[0];
        policy_post.viewer_blocked_by = json_bool(viewer, "blockedBy");
        policy_post.viewer_muted = json_bool(viewer, "muted");
    }

    cJSON *reason = cJSON_GetObjectItemCaseSensitive(item, "reason");
    if (cJSON_IsObject(reason)) {
        const char *reason_type = json_string(reason, "$type");
        policy_post.is_repost =
            reason_type && std::string_view(reason_type) == "app.bsky.feed.defs#reasonRepost";
    }

    const PolicyDecision decision = evaluate_post(account_did, policy_post);
    out.author_did = policy_post.author_did;
    out.created_at = json_string(record, "createdAt") ? json_string(record, "createdAt") : "";
    out.policy_reason = decision.reason;
    /* Skipped items carry empty text so the ledger records them as SKIPPED
     * — observed but not learned, distinguishable from never-fetched. */
    out.text = decision.eligible && text ? std::string(text) : std::string{};
    return true;
}

} // namespace atperson
