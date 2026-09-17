#include "client.hpp"

#include <cJSON.h>
#include <wolfram/agent.h>
#include <wolfram/xrpc.h>

#include "policy.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

namespace {

struct ResponseGuard {
    wf_response response{};

    ~ResponseGuard() {
        wf_response_free(&response);
    }
};

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

const char *json_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : nullptr;
}

bool json_bool(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

std::runtime_error wolfram_error(std::string_view operation, wf_status status) {
    return std::runtime_error(std::string(operation) +
                              " failed with Wolfram status " +
                              std::to_string(static_cast<int>(status)));
}

} // namespace

TimelineHttpError::TimelineHttpError(long status, const std::string &message)
    : std::runtime_error(message), status_(status) {}

AtprotoClient::AtprotoClient(std::string service, std::string identifier,
                             std::string app_password)
    : agent_(wf_agent_new(service.c_str())) {
    if (!agent_) {
        throw std::runtime_error("failed to create Wolfram agent");
    }

    const wf_status status =
        wf_agent_login(agent_.get(), identifier.c_str(), app_password.c_str());
    if (status != WF_OK) {
        throw wolfram_error("AT Protocol login", status);
    }

    const char *did = wf_agent_get_did(agent_.get());
    if (!did || !did[0]) {
        throw std::runtime_error("AT Protocol login did not yield an account DID");
    }
    did_ = did;
}

std::string AtprotoClient::account_did() const {
    return did_;
}

SyncPage AtprotoClient::fetch_timeline_page(const std::optional<std::string> &cursor,
                                            int limit) {
    limit = std::clamp(limit, 1, 100);

    ResponseGuard response;
    const wf_status status = wf_agent_get_timeline(
        agent_.get(), limit, cursor ? cursor->c_str() : nullptr, nullptr,
        &response.response);
    if (status == WF_ERR_HTTP) {
        // The service rejected the request. A persisted cursor that the
        // server no longer accepts lands here; the caller resets to the
        // head and relies on ledger dedup.
        const char *error = wf_agent_last_error(agent_.get());
        throw TimelineHttpError(
            response.response.status,
            std::string("timeline fetch rejected with HTTP ") +
                std::to_string(response.response.status) +
                (error ? std::string(": ") + error : std::string()));
    }
    if (status != WF_OK) {
        throw wolfram_error("timeline fetch", status);
    }
    if (!response.response.body) {
        throw std::runtime_error("timeline fetch returned an empty response");
    }

    Json root(cJSON_ParseWithLength(response.response.body,
                                    response.response.body_len));
    if (!root) {
        throw std::runtime_error("timeline response was not valid JSON");
    }

    cJSON *feed = cJSON_GetObjectItemCaseSensitive(root.get(), "feed");
    if (!cJSON_IsArray(feed)) {
        throw std::runtime_error("timeline response did not contain a feed");
    }

    SyncPage page;
    page.items.reserve(static_cast<std::size_t>(cJSON_GetArraySize(feed)));

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, feed) {
        cJSON *post = cJSON_GetObjectItemCaseSensitive(item, "post");
        if (!cJSON_IsObject(post)) {
            continue;
        }

        cJSON *record = cJSON_GetObjectItemCaseSensitive(post, "record");
        cJSON *author = cJSON_GetObjectItemCaseSensitive(post, "author");
        if (!cJSON_IsObject(record)) {
            continue;
        }

        const char *text = json_string(record, "text");
        const char *uri = json_string(post, "uri");
        if (!uri) {
            continue;
        }

        /* Extract the fields the ingestion policy decides on. */
        PolicyPost policy_post;
        const char *record_type = json_string(record, "$type");
        policy_post.record_type = record_type ? record_type : "";
        const char *did = cJSON_IsObject(author) ? json_string(author, "did") : nullptr;
        policy_post.author_did = did ? did : "";
        policy_post.text = text ? std::string_view(text) : std::string_view{};

        cJSON *viewer = cJSON_GetObjectItemCaseSensitive(post, "viewer");
        if (cJSON_IsObject(viewer)) {
            const char *blocking = json_string(viewer, "blocking");
            policy_post.viewer_blocked = blocking && blocking[0];
            policy_post.viewer_blocked_by = json_bool(viewer, "blockedBy");
            policy_post.viewer_muted = json_bool(viewer, "muted");
        }

        cJSON *embed = cJSON_GetObjectItemCaseSensitive(record, "embed");
        if (cJSON_IsObject(embed)) {
            const char *embed_type = json_string(embed, "$type");
            policy_post.embed_type = embed_type ? embed_type : "";
            /* A quote-post embed carries the quoted record's text; the
             * record's own text remains the learnable content here. */
            policy_post.embed_has_text_fallback =
                policy_post.embed_type == "app.bsky.embed.record";
        }

        cJSON *reason = cJSON_GetObjectItemCaseSensitive(item, "reason");
        if (cJSON_IsObject(reason)) {
            const char *reason_type = json_string(reason, "$type");
            policy_post.is_repost =
                reason_type && std::string_view(reason_type) == "app.bsky.feed.defs#reasonRepost";
        }
        policy_post.is_reply = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(item, "reply"));

        const PolicyDecision decision = evaluate_post(did_, policy_post);
        if (!decision.eligible) {
            /* Skipped by policy, not silently: the item still becomes an
             * observation with an empty text so the ledger records it as
             * SKIPPED and it can be distinguished from never-fetched data. */
            page.items.push_back(SyncObservation{
                .text = "",
                .source_uri = uri,
                .author_did = policy_post.author_did,
                .created_at = json_string(record, "createdAt") ? json_string(record, "createdAt") : "",
                .policy_reason = decision.reason,
            });
            continue;
        }

        page.items.push_back(SyncObservation{
            .text = text ? text : "",
            .source_uri = uri,
            .author_did = policy_post.author_did,
            .created_at = json_string(record, "createdAt") ? json_string(record, "createdAt") : "",
            .policy_reason = decision.reason,
        });
    }

    // The cursor is opaque: read it, pass it back to Wolfram, never parse it.
    char *next = nullptr;
    if (wf_response_cursor(&response.response, &next) == WF_OK && next) {
        page.next_cursor = std::string(next);
        std::free(next);
    }
    return page;
}

} // namespace atperson
