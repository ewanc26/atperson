#include "atproto/jetstream_replay.hpp"

#include "wolfram/jetstream_replay.h"

#include <cassert>
#include <cstdlib>
#include <cstring>

using namespace atperson;

int main() {
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

    free(event.collection);
    free(event.did);
    free(event.rkey);
    free(event.rev);
    free(event.payload);
    return 0;
}
