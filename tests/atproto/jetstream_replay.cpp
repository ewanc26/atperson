#include "atproto/jetstream_replay.hpp"
#include "atproto/jetstream_replay_client.hpp"

#include "wolfram/jetstream_replay.h"
#include "wolfram/repo/cbor.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace atperson;

namespace {

void cbor_append_string(std::string &out, const std::string_view value) {
    const std::size_t n = value.size();
    if (n < 24u) {
        out.push_back(static_cast<char>(0x60u | n));
    } else {
        out.push_back(static_cast<char>(0x78u));
        out.push_back(static_cast<char>(n));
    }
    out.append(value.data(), n);
}

/* DAG-CBOR map keys must be byte-wise sorted; callers pass pairs in canonical
 * order, which Wolfram's canonical round-trip check enforces anyway. Map and
 * string headers use the same style the archive emits. */
void cbor_append_map(
    std::string &out,
    const std::pair<std::string_view, std::string_view> *pairs,
    std::size_t count) {
    if (count < 24u) {
        out.push_back(static_cast<char>(0xa0u | count));
    } else {
        out.push_back(static_cast<char>(0xb8u));
        out.push_back(static_cast<char>(count));
    }
    for (std::size_t i = 0u; i < count; ++i) {
        cbor_append_string(out, pairs[i].first);
        cbor_append_string(out, pairs[i].second);
    }
}

/* A map whose values are already-encoded CBOR, so fixtures can nest. Keys must
 * be passed in canonical bytewise order for the same reason as above. */
void cbor_append_map_raw(
    std::string &out,
    const std::pair<std::string_view, std::string> *pairs, std::size_t count) {
    if (count < 24u) {
        out.push_back(static_cast<char>(0xa0u | count));
    } else {
        out.push_back(static_cast<char>(0xb8u));
        out.push_back(static_cast<char>(count));
    }
    for (std::size_t i = 0u; i < count; ++i) {
        cbor_append_string(out, pairs[i].first);
        out.append(pairs[i].second);
    }
}

void cbor_append_string_array(std::string &out,
                              const std::vector<std::string_view> &values) {
    if (values.size() < 24u) {
        out.push_back(static_cast<char>(0x80u | values.size()));
    } else {
        out.push_back(static_cast<char>(0x98u));
        out.push_back(static_cast<char>(values.size()));
    }
    for (const std::string_view value : values) {
        cbor_append_string(out, value);
    }
}

/* A DAG-CBOR CID link: tag 42 wrapping the multibase identity prefix, a
 * CIDv1 header and a sha2-256 digest. Wolfram validates that structure before
 * it will return a link at all, so the fixture has to carry a real CID shape.
 * The decoder has no JSON form for it, so the field is skipped rather than
 * mangled. */
void cbor_append_link(std::string &out) {
    out.push_back(static_cast<char>(0xd8u)); /* tag */
    out.push_back(static_cast<char>(0x2au)); /* 42 */
    out.push_back(static_cast<char>(0x58u)); /* byte string, 1-byte length */
    out.push_back(static_cast<char>(0x23u)); /* 35 */
    out.push_back(static_cast<char>(0x00u)); /* multibase identity prefix */
    out.push_back(static_cast<char>(0x12u)); /* cidv1, sha2-256 */
    out.push_back(static_cast<char>(0x20u)); /* 32-byte digest */
    for (int i = 0; i < 32; ++i) {
        out.push_back(static_cast<char>(i));
    }
}

wf_jetstream_replay_event make_commit_event(const std::string &payload) {
    wf_jetstream_replay_event event{};
    event.seq = 43u;
    event.witnessed_at = 1779364800000000LL;
    event.kind = 1u;
    event.collection = strdup("app.bsky.feed.post");
    event.did = strdup("did:plc:archive");
    event.rkey = strdup("3k");
    event.rev = strdup("rev");
    event.payload = static_cast<unsigned char *>(malloc(payload.size()));
    event.payload_len = payload.size();
    assert(event.collection && event.did && event.rkey && event.rev && event.payload);
    std::memcpy(event.payload, payload.data(), payload.size());
    return event;
}

void free_commit_event(wf_jetstream_replay_event &event) {
    free(event.collection);
    free(event.did);
    free(event.rkey);
    free(event.rev);
    free(event.payload);
}

} // namespace

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

    /* Archive rows carry record payloads as DAG-CBOR, not JSON. A real post
     * record must decode into the same event the JSON path produces, and the
     * dropped counter must stay zero. This is the regression for the silent
     * archive drop: the payload is a CBOR map (0xa3) exactly what the archive
     * serves.
     *
     * Key order is canonical bytewise over the encoded key, not human order:
     * "text" (0x64) sorts before "$type" (0x65) before "createdAt" (0x69).
     * Wolfram's parser re-serialises and byte-compares, so a non-canonical
     * map is rejected outright rather than decoded. */
    const std::pair<std::string_view, std::string_view> cbor_pairs[] = {
        {"text", "cbor archive post"},
        {"$type", "app.bsky.feed.post"},
        {"createdAt", "2026-09-20T13:00:00Z"},
    };
    std::string cbor_bytes;
    cbor_append_map(cbor_bytes, cbor_pairs, 3u);
    assert(cbor_bytes.size() > 0u &&
           static_cast<unsigned char>(cbor_bytes[0]) == 0xa3u);
    wf_cbor_item *parsed = wf_cbor_parse(
        reinterpret_cast<const unsigned char *>(cbor_bytes.data()), cbor_bytes.size());
    assert(parsed != nullptr && parsed->type == WF_CBOR_MAP &&
           parsed->map.count == 3u);
    wf_cbor_free(parsed);

    wf_jetstream_replay_event cbor_event = make_commit_event(cbor_bytes);
    int cbor_delivered = 0;
    std::size_t cbor_dropped = 0u;
    translate_jetstream_replay_events(
        &cbor_event, 1u, "did:plc:self",
        [&](const JetstreamEvent &translated) {
            ++cbor_delivered;
            assert(translated.source_uri ==
                   "at://did:plc:archive/app.bsky.feed.post/3k");
            assert(translated.text == "cbor archive post");
            assert(translated.seq == 43);
        },
        &cbor_dropped);
    assert(cbor_delivered == 1);
    assert(cbor_dropped == 0u);
    free_commit_event(cbor_event);

    /* A real archive reply record: nested strongRefs, each with a CID link the
     * decoder has no JSON form for, plus a langs array. If skipping the link
     * failed the whole record instead of the field, every reply in the archive
     * would be dropped and conversation context would never be learned. This
     * must still deliver, with the uris intact. */
    auto encode_strongref = [](const std::string_view uri) {
        std::string link;
        cbor_append_link(link);
        std::string uri_str;
        cbor_append_string(uri_str, uri);
        const std::pair<std::string_view, std::string> fields[] = {
            {"cid", link}, {"uri", uri_str},
        };
        std::string out;
        cbor_append_map_raw(out, fields, 2u);
        return out;
    };
    const std::string root_value = encode_strongref(
        "at://did:plc:archive/app.bsky.feed.post/1a");
    const std::string parent_value = encode_strongref(
        "at://did:plc:archive/app.bsky.feed.post/2b");
    std::string langs_value;
    cbor_append_string_array(langs_value, {"en"});
    const std::pair<std::string_view, std::string> reply_refs[] = {
        {"root", root_value},
        {"parent", parent_value},
    };
    std::string reply_value;
    cbor_append_map_raw(reply_value, reply_refs, 2u);

    std::string reply_text;
    cbor_append_string(reply_text, "nested reply text");
    std::string type_value;
    cbor_append_string(type_value, "app.bsky.feed.post");
    std::string created_value;
    cbor_append_string(created_value, "2026-09-20T13:00:00Z");

    const std::pair<std::string_view, std::string> reply_pairs[] = {
        {"text", reply_text},
        {"$type", type_value},
        {"langs", langs_value},
        {"reply", reply_value},
        {"createdAt", created_value},
    };
    std::string reply_bytes;
    cbor_append_map_raw(reply_bytes, reply_pairs, 5u);

    wf_cbor_item *reply_parsed = wf_cbor_parse(
        reinterpret_cast<const unsigned char *>(reply_bytes.data()),
        reply_bytes.size());
    assert(reply_parsed != nullptr && reply_parsed->type == WF_CBOR_MAP &&
           reply_parsed->map.count == 5u);
    wf_cbor_free(reply_parsed);

    wf_jetstream_replay_event reply_event = make_commit_event(reply_bytes);
    int reply_delivered = 0;
    std::size_t reply_dropped = 0u;
    translate_jetstream_replay_events(
        &reply_event, 1u, "did:plc:self",
        [&](const JetstreamEvent &translated) {
            ++reply_delivered;
            assert(translated.text == "nested reply text");
            assert(translated.reply_root ==
                   "at://did:plc:archive/app.bsky.feed.post/1a");
            assert(translated.reply_parent ==
                   "at://did:plc:archive/app.bsky.feed.post/2b");
        },
        &reply_dropped);
    assert(reply_delivered == 1);
    assert(reply_dropped == 0u);
    free_commit_event(reply_event);

    /* A row whose payload is neither JSON nor decodable CBOR must be counted
     * as dropped, not silently erased. */
    const std::string garbage("\xff\x00\x00", 3u);
    wf_jetstream_replay_event bad_event = make_commit_event(garbage);
    int bad_delivered = 0;
    std::size_t bad_dropped = 0u;
    translate_jetstream_replay_events(
        &bad_event, 1u, "did:plc:self",
        [&](const JetstreamEvent &) { ++bad_delivered; }, &bad_dropped);
    assert(bad_delivered == 0);
    assert(bad_dropped == 1u);
    free_commit_event(bad_event);

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

    /* Auth-rejection contract: a rejected credential must map to a distinct,
     * greppable message on every archive call, so a dead or expired token
     * fails the backfill fast instead of being retried as a transient
     * transport failure. Every other status keeps the generic wording.
     *
     * The mapping is asserted directly rather than through a loopback server:
     * a 401 arriving as WF_ERR_AUTH is wolfram's contract, and driving a real
     * socket here made this test hang on the hosted runners. */
    const char *operations[] = {"tip probe", "planSnapshot", "getSegment",
                                "getBlock"};
    for (const char *operation : operations) {
        const std::string rejected =
            jetstream_archive_error(operation, WF_ERR_AUTH);
        assert(rejected.find("WF_ERR_AUTH") != std::string::npos);
        assert(rejected.find("ATPERSON_JETSTREAM_ARCHIVE_TOKEN") !=
               std::string::npos);
        assert(rejected.find(operation) != std::string::npos);
        /* A transient failure must stay distinguishable from a dead token, or
         * the retry loop treats the two the same. */
        for (const int status : {WF_ERR_NETWORK, WF_ERR_HTTP, WF_ERR_TIMEOUT}) {
            const std::string transient_message =
                jetstream_archive_error(operation, status);
            assert(transient_message.find("WF_ERR_AUTH") == std::string::npos);
            assert(transient_message.find(
                       "ATPERSON_JETSTREAM_ARCHIVE_TOKEN") == std::string::npos);
        }
    }
    assert(jetstream_archive_error("tip probe", WF_OK) ==
           "Jetstream replay tip probe failed");
    assert(jetstream_archive_error("getBlock", WF_ERR_AUTH) ==
           "Jetstream archive getBlock failed: archive rejected credentials "
           "(WF_ERR_AUTH) — check ATPERSON_JETSTREAM_ARCHIVE_TOKEN");

    free(event.collection);
    free(event.did);
    free(event.rkey);
    free(event.rev);
    free(event.payload);
    return 0;
}
