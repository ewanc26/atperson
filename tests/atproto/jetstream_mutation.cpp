/* Totality of the Jetstream commit extractor on hostile input. The bytes come
 * off a public firehose, so extract_jetstream_commit must, for every input,
 * return true/false without crashing, hanging or unbounded allocation, and
 * anything it accepts must be well formed (a post URI, a DID, non-empty text).
 * Covers hostile shapes and a deterministic mutation sweep. Offline. */

#include "atproto/jetstream.hpp"
#include "sync/engine.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atperson::SyncObservation;

constexpr std::string_view kSelf = "did:plc:self";

const std::string kValidPost = R"({
    "did": "did:plc:author",
    "time_us": 1726200000123456,
    "kind": "commit",
    "commit": {
        "rev": "3l3qo2vutsw2b",
        "operation": "create",
        "collection": "app.bsky.feed.post",
        "rkey": "3l3qo2vuowo2b",
        "record": {
            "$type": "app.bsky.feed.post",
            "text": "the silver moon over the quiet harbour",
            "createdAt": "2026-09-17T00:00:00Z",
            "langs": ["en"],
            "reply": {
                "root": {"uri": "at://did:plc:root/app.bsky.feed.post/3k0", "cid": "bafyroot"},
                "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k1", "cid": "bafyparent"}
            },
            "embed": {
                "$type": "app.bsky.embed.record",
                "record": {"uri": "at://did:plc:quoted/app.bsky.feed.post/3k2", "cid": "bafyquote"}
            }
        },
        "cid": "bafyreicommit"
    }
})";

bool extract(const std::string &json, SyncObservation &out) {
    return atperson::extract_jetstream_commit(json.data(), json.size(), kSelf, out);
}

/* An accepted observation always names its record and author; one the policy
 * marks eligible must also carry text to learn. A non-eligible reason (empty
 * text, self-authored, ...) is the extractor reporting why it will not be
 * learned, which is a valid outcome. */
void expect_well_formed(const SyncObservation &observation) {
    if (observation.source_uri.rfind("at://", 0) != 0 ||
        observation.author_did.rfind("did:", 0) != 0) {
        std::fprintf(stderr, "accepted malformed identity: author='%s' uri='%s'\n",
                     observation.author_did.c_str(), observation.source_uri.c_str());
    }
    assert(observation.source_uri.rfind("at://", 0) == 0);
    assert(observation.author_did.rfind("did:", 0) == 0);
    if (observation.policy_reason == atperson::PolicyReason::Eligible) {
        assert(!observation.text.empty());
    }
}

void test_valid_post_is_accepted() {
    SyncObservation observation;
    assert(extract(kValidPost, observation));
    expect_well_formed(observation);
    assert(observation.text == "the silver moon over the quiet harbour");
    assert(observation.author_did == "did:plc:author");
}

/* The commit's identity fields are spliced into the source URI and stored as
 * the author, so only well-formed DIDs and record keys may pass. */
void test_malformed_identities_are_rejected() {
    auto with = [](const std::string &did, const std::string &rkey) {
        std::string json = kValidPost;
        json.replace(json.find("did:plc:author"), std::string("did:plc:author").size(), did);
        const std::string marker = "\"rkey\": \"";
        const std::size_t begin = json.find(marker) + marker.size();
        const std::size_t end = json.find('"', begin);
        json.replace(begin, end - begin, rkey);
        return json;
    };
    SyncObservation observation;
    assert(extract(with("did:plc:author", "3l3qo2vuowo2b"), observation));
    /* Missing separator, delimiter and control characters, bad method,
     * trailing colon, non-ASCII. */
    for (const char *did : {"didplc:author", "did:plc:", "did:PLC:author", "did:plc:aut hor",
                            "did:plc:au/thor", "did:plc:au#thor", "did:plc:author:", "did:plc:\\u00e9",
                            "did:plc:au\\nthor", "did:plc:au\\\"thor", "author", "did:"}) {
        SyncObservation item;
        assert(!extract(with(did, "3l3qo2vuowo2b"), item));
    }
    /* A DID longer than the syntax allows. */
    assert(!extract(with("did:plc:" + std::string(2100u, 'a'), "3l3qo2vuowo2b"), observation));
    /* Record keys that would corrupt or escape the AT URI. */
    for (const char *rkey : {"..", ".", "a/b", "a#b", "a?b", "a b", "a\\nb", "a\\u00e9"}) {
        SyncObservation item;
        assert(!extract(with("did:plc:author", rkey), item));
    }
    assert(!extract(with("did:plc:author", std::string(513u, 'a')), observation));
    /* Legitimate variants are still accepted. */
    assert(extract(with("did:web:example.com", "self"), observation));
    assert(extract(with("did:plc:author", "a.b_c-d:e~f"), observation));
}

void test_hostile_shapes_are_rejected_not_fatal() {
    SyncObservation observation;
    assert(!atperson::extract_jetstream_commit(nullptr, 0u, kSelf, observation));
    assert(!extract("", observation));
    assert(!extract("null", observation));
    assert(!extract("[]", observation));
    assert(!extract("\"text\"", observation));
    assert(!extract("{", observation));
    assert(!extract("{\"did\":", observation));
    assert(!extract("{\"kind\":\"commit\"}", observation));

    /* Deep nesting is bounded by the JSON parser, not by the C++ stack. */
    for (const std::size_t depth : {100u, 1000u, 100000u}) {
        std::string nested(depth, '[');
        nested.append(depth, ']');
        assert(!extract(nested, observation));
        std::string in_record = kValidPost;
        const std::size_t at = in_record.find("\"langs\": [\"en\"]");
        assert(at != std::string::npos);
        in_record.replace(at, std::string("\"langs\": [\"en\"]").size(),
                          "\"langs\": " + std::string(depth, '[') + std::string(depth, ']'));
        (void)extract(in_record, observation); /* must simply return */
    }

    /* Wrong types where strings/objects are expected. */
    for (const char *replacement : {"123", "true", "null", "[]", "{}", "\"\""}) {
        std::string variant = kValidPost;
        const std::size_t at = variant.find("\"the silver moon over the quiet harbour\"");
        variant.replace(at, std::string("\"the silver moon over the quiet harbour\"").size(),
                        replacement);
        SyncObservation item;
        if (extract(variant, item)) {
            expect_well_formed(item);
        }
    }

    /* Embedded NUL and invalid UTF-8 inside the text must not crash. */
    std::string with_nul = kValidPost;
    with_nul.replace(with_nul.find("silver"), 6u, std::string("sil\0er", 6u));
    SyncObservation nul_item;
    if (extract(with_nul, nul_item)) {
        expect_well_formed(nul_item);
    }
    std::string bad_utf8 = kValidPost;
    bad_utf8.replace(bad_utf8.find("silver"), 6u, "\xff\xfe\xc0\x80\xed\xa0");
    SyncObservation utf_item;
    if (extract(bad_utf8, utf_item)) {
        expect_well_formed(utf_item);
    }

    /* A very large text field is handled without pathological cost. */
    std::string huge = kValidPost;
    huge.replace(huge.find("the silver moon over the quiet harbour"),
                 std::string("the silver moon over the quiet harbour").size(),
                 std::string(4u * 1024u * 1024u, 'a'));
    SyncObservation huge_item;
    const auto start = std::chrono::steady_clock::now();
    if (extract(huge, huge_item)) {
        expect_well_formed(huge_item);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    assert(ms < 2000);
}

void test_mutation_sweep_is_total() {
    std::uint64_t state = 0xD1B54A32D192ED03ull;
    auto next = [&state]() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    };
    std::size_t accepted = 0u;
    const auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 40000; ++iteration) {
        std::string mutated = kValidPost;
        const unsigned edits = 1u + static_cast<unsigned>(next() % 5u);
        for (unsigned e = 0u; e < edits && !mutated.empty(); ++e) {
            const std::size_t at = static_cast<std::size_t>(next() % mutated.size());
            switch (next() % 5u) {
            case 0u:
                mutated[at] = static_cast<char>(next());
                break;
            case 1u:
                mutated.insert(mutated.begin() + static_cast<std::ptrdiff_t>(at),
                               static_cast<char>(next()));
                break;
            case 2u:
                mutated.erase(mutated.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            case 3u:
                mutated.resize(at);
                break;
            default: {
                /* splice a structural character, the mutation most likely to
                 * change the JSON shape rather than just a value */
                static constexpr char kStructural[] = "{}[]\",:\\";
                mutated[at] = kStructural[next() % (sizeof kStructural - 1u)];
                break;
            }
            }
        }
        SyncObservation observation;
        if (extract(mutated, observation)) {
            ++accepted;
            expect_well_formed(observation);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    assert(accepted > 0u);   /* value-only edits stay valid, so the sweep is meaningful */
    assert(ms < 20000);      /* and total work stays bounded */
}

} // namespace

int main() {
    test_valid_post_is_accepted();
    test_malformed_identities_are_rejected();
    test_hostile_shapes_are_rejected_not_fatal();
    test_mutation_sweep_is_total();
    std::puts("jetstream mutation tests passed");
    return 0;
}
