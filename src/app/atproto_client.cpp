#include "atproto_client.hpp"

#include <cJSON.h>
#include <wolfram/agent.h>
#include <wolfram/xrpc.h>

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
    return cJSON_IsString(value) && value->valuestring ? value->valuestring
                                                       : nullptr;
}

std::runtime_error wolfram_error(std::string_view operation, wf_status status) {
    return std::runtime_error(std::string(operation) +
                              " failed with Wolfram status " +
                              std::to_string(static_cast<int>(status)));
}

} // namespace

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
}

std::vector<NetworkObservation> AtprotoClient::fetch_timeline(int limit) {
    limit = std::clamp(limit, 1, 100);

    ResponseGuard response;
    const wf_status status =
        wf_agent_get_timeline(agent_.get(), limit, nullptr, nullptr,
                              &response.response);
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

    std::vector<NetworkObservation> observations;
    observations.reserve(static_cast<std::size_t>(cJSON_GetArraySize(feed)));

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
        const char *did = cJSON_IsObject(author) ? json_string(author, "did")
                                                  : nullptr;
        const char *created_at = json_string(record, "createdAt");

        if (!text || !uri) {
            continue;
        }

        observations.push_back(NetworkObservation{
            .text = text,
            .source_uri = uri,
            .author_did = did ? did : "",
            .created_at = created_at ? created_at : "",
        });
    }

    return observations;
}

} // namespace atperson
