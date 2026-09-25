#include "atproto/jetstream_replay.hpp"
#include "atproto/jetstream_replay_client.hpp"

#include "wolfram/jetstream_replay.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>

using namespace atperson;

int main() {
    assert(bounded_jetstream_replay_before(41u, std::nullopt) ==
           std::optional<std::uint64_t>(41u + kJetstreamArchiveMaxSequenceSpan));
    assert(bounded_jetstream_replay_before(41u, 99u) ==
           std::optional<std::uint64_t>(99u));
    assert(!bounded_jetstream_replay_before(41u, 41u));
    assert(!bounded_jetstream_replay_before(
        41u, 41u + kJetstreamArchiveMaxSequenceSpan + 1u));
    assert(bounded_jetstream_replay_before(
        std::numeric_limits<std::uint64_t>::max() -
            kJetstreamArchiveMaxSequenceSpan + 1u,
        std::nullopt) ==
           std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()));
    const char payload[] =
        "{\"$type\":\"app.bsky.feed.post\",\"text\":\"archive post\","
        "\"createdAt\":\"2026-09-20T12:00:00Z\"}";
    wf_jetstream_replay_event event{};
    event.seq = 42u;
    event.witnessed_at = 1779364800000000LL;
    event.kind = 1u;
    event.collection = strdup("app.bsky.feed.post");
    event.did = strdup("did:plc:archive");
    event.rkey = strdup("3k");
    event.rev = strdup("rev");
    event.payload = static_cast<unsigned char *>(malloc(sizeof(payload) - 1u));
    event.payload_len = sizeof(payload) - 1u;
    assert(event.collection && event.did && event.rkey && event.rev && event.payload);
    std::memcpy(event.payload, payload, event.payload_len);

    int delivered = 0;
    translate_jetstream_replay_events(
        &event, 1u, "did:plc:self", [&](const JetstreamEvent &translated) {
            ++delivered;
            assert(translated.source_uri ==
                   "at://did:plc:archive/app.bsky.feed.post/3k");
            assert(translated.text == "archive post");
            assert(translated.seq == 42);
        });
    assert(delivered == 1);

    /* Kind 7 (create_resync) is a commit create re-witnessed during a server
     * repository resync; it must enter the learning path as a create. */
    event.kind = 7u;
    int resync_delivered = 0;
    translate_jetstream_replay_events(
        &event, 1u, "did:plc:self", [&](const JetstreamEvent &translated) {
            ++resync_delivered;
            assert(translated.source_uri ==
                   "at://did:plc:archive/app.bsky.feed.post/3k");
            assert(translated.text == "archive post");
            assert(translated.seq == 42);
        });
    assert(resync_delivered == 1);
    event.kind = 1u;

    const char *kinds[] = {"commit"};
    const char *collections[] = {"app.bsky.feed.post", "app.bsky.feed.like"};
    const char *dids[] = {"did:plc:archive"};
    wf_jetstream_replay_filter filter{};
    filter.kinds = kinds;
    filter.kinds_count = 1u;
    filter.collections = collections;
    filter.collections_count = 2u;
    filter.dids = dids;
    filter.dids_count = 1u;
    filter.after_seq = 41u;
    filter.before_seq = 99u;
    filter.has_before_seq = 1;
    char *json = nullptr;
    size_t json_len = 0u;
    assert(wf_jetstream_replay_plan_json(&filter, &json, &json_len) == WF_OK);
    assert(json != nullptr && json_len != 0u);
    const std::string request(json, json_len);
    assert(request.find("app.bsky.feed.like") != std::string::npos);
    assert(request.find("did:plc:archive") != std::string::npos);
    assert(request.find("41") != std::string::npos);
    assert(request.find("99") != std::string::npos);
    free(json);

    /* Archive client contract: the raw archive token is required, and the
     * transport is bound to the archive host (never the PDS session client).
     * Construction performs no network I/O, so both cases are offline-safe. */
    bool threw = false;
    try {
        const JetstreamReplayClient missing_token("", "");
        (void)missing_token;
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    const JetstreamReplayClient defaults_host("", "token");
    (void)defaults_host;

    /* Auth-rejection contract: a 401 from the archive must surface as a
     * distinct WF_ERR_AUTH error (greppable, fail-fast) rather than the
     * generic transport failure a retry loop would treat as transient.
     * Served from a loopback socket so no real archive is contacted. */
    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(listen_fd, reinterpret_cast<const sockaddr *>(&addr),
                sizeof(addr)) == 0);
    assert(listen(listen_fd, 8) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr),
                       &addr_len) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    std::thread reject_server([listen_fd] {
        for (int served = 0; served < 4; ++served) {
            const int fd = accept(listen_fd, nullptr, nullptr);
            if (fd < 0) break;
            /* The client sends exactly one request per probe; a second
             * accept would only be reached if the transport retried, which
             * is itself contract-relevant but not what this test pins. */
            char request[2048];
            (void)read(fd, request, sizeof(request));
            const std::string body =
                "{\"error\":\"InvalidToken\",\"message\":\"rejected\"}";
            const std::string response =
                "HTTP/1.1 401 Unauthorized\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            (void)write(fd, response.data(), response.size());
            close(fd);
        }
    });
    bool auth_threw = false;
    try {
        JetstreamReplayClient rejected(
            "http://127.0.0.1:" + std::to_string(port), "dead-token");
        (void)rejected.probe_sealed_tip();
    } catch (const std::runtime_error &e) {
        const std::string message(e.what());
        auth_threw = message.find("WF_ERR_AUTH") != std::string::npos &&
                     message.find("ATPERSON_JETSTREAM_ARCHIVE_TOKEN") !=
                         std::string::npos;
    }
    assert(auth_threw);
    close(listen_fd); /* unblocks the server thread's accept */
    reject_server.join();

    free(event.collection);
    free(event.did);
    free(event.rkey);
    free(event.rev);
    free(event.payload);
    return 0;
}
