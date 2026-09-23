/* Jetstream zstd dictionary fetch. See jetstream_dictionary.hpp for the
 * contract. This is the only module that knows how the dictionary is
 * obtained; the client treats it as opaque bytes. */

#include "jetstream_dictionary.hpp"

#include <wolfram/xrpc.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace atperson {
namespace {

struct ClientDelete {
    void operator()(wf_xrpc_client *client) const noexcept {
        wf_xrpc_client_free(client);
    }
};

struct ResponseGuard {
    wf_response response{};

    ~ResponseGuard() {
        wf_response_free(&response);
    }
};

} // namespace

std::string fetch_jetstream_zstd_dictionary(std::string_view service_base_url) {
    if (service_base_url.empty() ||
        service_base_url.rfind("https://", 0u) != 0u) {
        throw std::runtime_error(
            "jetstream dictionary: service base URL must be absolute https");
    }
    while (service_base_url.back() == '/') {
        service_base_url.remove_suffix(1u);
    }

    const std::unique_ptr<wf_xrpc_client, ClientDelete> client(
        wf_xrpc_client_new(std::string(service_base_url).c_str()));
    if (!client) {
        throw std::runtime_error(
            "jetstream dictionary: failed to create HTTP transport");
    }

    const std::string url =
        std::string(service_base_url) +
        "/xrpc/network.bsky.jetstream.getZstdDictionary";
    ResponseGuard guard;
    const wf_status status = wf_http_get(client.get(), url.c_str(), &guard.response);
    if (status != WF_OK) {
        throw std::runtime_error(
            "jetstream dictionary: getZstdDictionary request failed (native status " +
            std::to_string(static_cast<int>(status)) + ")");
    }
    if (guard.response.status != 200) {
        throw std::runtime_error(
            "jetstream dictionary: getZstdDictionary returned HTTP " +
            std::to_string(guard.response.status));
    }
    if (guard.response.body == nullptr || guard.response.body_len == 0u) {
        throw std::runtime_error(
            "jetstream dictionary: getZstdDictionary returned an empty body");
    }
    return std::string(guard.response.body, guard.response.body_len);
}

} // namespace atperson
