#include "atproto/jetstream_replay.hpp"
#include "atproto/jetstream_replay_client.hpp"

#include "wolfram/jetstream_replay.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

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

    free(event.collection);
    free(event.did);
    free(event.rkey);
    free(event.rev);
    free(event.payload);
    return 0;
}
