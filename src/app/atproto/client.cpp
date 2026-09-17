#include "client.hpp"

#include <cJSON.h>
#include <wolfram/agent.h>
#include <wolfram/xrpc.h>

#include "extract.hpp"
#include "session.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

} // namespace

TimelineHttpError::TimelineHttpError(long status, const std::string &message)
    : std::runtime_error(message), status_(status) {}

AtprotoClient::AtprotoClient(std::string service, std::string identifier,
                             std::string app_password)
    : session_(std::move(service), std::move(identifier), std::move(app_password)) {}

std::string AtprotoClient::account_did() const {
    return session_.did();
}

SyncPage AtprotoClient::fetch_timeline_page(const std::optional<std::string> &cursor,
                                            int limit) {
    limit = std::clamp(limit, 1, 100);

    ResponseGuard response;
    const wf_status status = wf_agent_get_timeline(
        session_.agent(), limit, cursor ? cursor->c_str() : nullptr, nullptr,
        &response.response);
    if (status == WF_ERR_HTTP) {
        // The service rejected the request. A persisted cursor that the
        // server no longer accepts lands here; the caller resets to the
        // head and relies on ledger dedup.
        const char *error = wf_agent_last_error(session_.agent());
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
        /* Translation lives in extract.cpp so it is offline-testable; this
         * loop only owns transport-adjacent iteration. Items too malformed
         * to reference anything are skipped entirely. */
        SyncObservation observation;
        if (extract_feed_item(item, session_.did(), observation)) {
            page.items.push_back(std::move(observation));
        }
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
