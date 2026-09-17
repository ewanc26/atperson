#include "audit/transport.hpp"

#include <wolfram/xrpc.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace atperson {
namespace audit {

namespace {

struct ResponseGuard {
    wf_response response{};

    ~ResponseGuard() {
        wf_response_free(&response);
    }
};

struct ClientDelete {
    void operator()(wf_xrpc_client *client) const noexcept {
        wf_xrpc_client_free(client);
    }
};

std::runtime_error http_error(const std::string &endpoint, long status,
                              const char *body, std::size_t body_len) {
    std::string message = "audit: TypeSafe endpoint " + endpoint + " returned HTTP " +
                          std::to_string(status);
    if (body && body_len > 0u) {
        constexpr std::size_t kMaxBody = 512u;
        const std::size_t shown = body_len < kMaxBody ? body_len : kMaxBody;
        message += ": " + std::string(body, shown);
    }
    if (status == 401 || status == 403) {
        message += " (check ATPERSON_TYPESAFE_API_KEY)";
    } else if (status == 429 || status == 529) {
        message += " (rate limited; retry after a short delay)";
    }
    return std::runtime_error(message);
}

} // namespace

std::string post_typesafe_evaluation(const std::string &endpoint, const std::string &api_key,
                                     const std::string &payload) {
    if (endpoint.empty() || api_key.empty()) {
        throw std::runtime_error("audit: endpoint and API key must not be empty");
    }

    std::unique_ptr<wf_xrpc_client, ClientDelete> client(
        wf_xrpc_client_new("https://api.typesafe.ai"));
    if (!client) {
        throw std::runtime_error("audit: failed to create HTTP transport");
    }

    const std::string authorization = "Bearer " + api_key;
    const wf_http_header headers[] = {{"Authorization", authorization.c_str()}};

    ResponseGuard response;
    const wf_status status = wf_http_post(
        client.get(), endpoint.c_str(), "application/json", payload.c_str(), headers, 1u,
        &response.response);
    if (status != WF_OK && status != WF_ERR_HTTP) {
        throw std::runtime_error("audit: TypeSafe request failed with Wolfram status " +
                                 std::to_string(static_cast<int>(status)));
    }
    if (response.response.status < 200 || response.response.status >= 300) {
        const char *body = response.response.body;
        throw http_error(endpoint, response.response.status, body,
                         body ? response.response.body_len : 0u);
    }
    if (!response.response.body) {
        throw std::runtime_error("audit: TypeSafe response had an empty body");
    }
    return std::string(response.response.body, response.response.body_len);
}

} // namespace audit
} // namespace atperson