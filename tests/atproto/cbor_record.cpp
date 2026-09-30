/* Hardening for the DAG-CBOR -> JSON record decoder. The bytes come from a
 * remote archive, so the decoder must be total: every input either yields a
 * JSON object or nullptr, never a crash, hang or unbounded recursion. Covers
 * targeted edge cases (shape, integer range, skipped fields, depth) and a
 * deterministic mutation sweep over a realistic record. Offline. */

#include "atproto/cbor_record.hpp"

#include <cJSON.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;

void head(Bytes &out, unsigned major, std::uint64_t argument) {
    const unsigned char base = static_cast<unsigned char>(major << 5u);
    if (argument < 24u) {
        out.push_back(base | static_cast<unsigned char>(argument));
    } else if (argument <= 0xFFu) {
        out.push_back(base | 24u);
        out.push_back(static_cast<unsigned char>(argument));
    } else if (argument <= 0xFFFFu) {
        out.push_back(base | 25u);
        out.push_back(static_cast<unsigned char>(argument >> 8u));
        out.push_back(static_cast<unsigned char>(argument));
    } else if (argument <= 0xFFFFFFFFu) {
        out.push_back(base | 26u);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<unsigned char>(argument >> shift));
        }
    } else {
        out.push_back(base | 27u);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<unsigned char>(argument >> shift));
        }
    }
}

void text(Bytes &out, const std::string &value) {
    head(out, 3u, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

cJSON *decode(const Bytes &bytes) {
    return atperson::decode_cbor_record(bytes.data(), bytes.size());
}

/* A realistic post in canonical DAG-CBOR key order (shorter keys first, then
 * bytewise) so the strict parser accepts it. */
Bytes sample_post() {
    Bytes out;
    head(out, 5u, 4u);
    text(out, "text");
    text(out, "hello harbour");
    text(out, "$type");
    text(out, "app.bsky.feed.post");
    text(out, "langs");
    head(out, 4u, 1u);
    text(out, "en");
    text(out, "createdAt");
    text(out, "2026-09-17T00:00:00Z");
    return out;
}

const char *string_field(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

void test_decodes_a_realistic_record() {
    cJSON *record = decode(sample_post());
    assert(record != nullptr && cJSON_IsObject(record));
    assert(std::strcmp(string_field(record, "text"), "hello harbour") == 0);
    assert(std::strcmp(string_field(record, "$type"), "app.bsky.feed.post") == 0);
    const cJSON *langs = cJSON_GetObjectItemCaseSensitive(record, "langs");
    assert(cJSON_IsArray(langs) && cJSON_GetArraySize(langs) == 1);
    cJSON_Delete(record);
}

void test_rejects_non_records_and_bad_framing() {
    assert(atperson::decode_cbor_record(nullptr, 0u) == nullptr);
    const unsigned char one = 0xa0;
    assert(atperson::decode_cbor_record(nullptr, 1u) == nullptr);
    assert(atperson::decode_cbor_record(&one, 0u) == nullptr);

    /* Empty map is a (useless but) valid record. */
    cJSON *empty = atperson::decode_cbor_record(&one, 1u);
    assert(empty != nullptr && cJSON_IsObject(empty) && empty->child == nullptr);
    cJSON_Delete(empty);

    Bytes array;
    head(array, 4u, 0u);
    assert(decode(array) == nullptr); /* not a map */
    Bytes string;
    text(string, "text");
    assert(decode(string) == nullptr);
    Bytes integer;
    head(integer, 0u, 7u);
    assert(decode(integer) == nullptr);

    Bytes trailing = sample_post();
    trailing.push_back(0x00);
    assert(decode(trailing) == nullptr); /* trailing bytes */

    Bytes truncated = sample_post();
    truncated.resize(truncated.size() / 2u);
    assert(decode(truncated) == nullptr);
    for (std::size_t cut = 0u; cut + 1u < sample_post().size(); ++cut) {
        Bytes prefix = sample_post();
        prefix.resize(cut);
        cJSON *partial = decode(prefix);
        cJSON_Delete(partial); /* any truncation is nullptr; never a crash */
        assert(partial == nullptr);
    }

    /* A non-string map key means these are not record bytes. */
    Bytes int_key;
    head(int_key, 5u, 1u);
    head(int_key, 0u, 1u);
    text(int_key, "v");
    assert(decode(int_key) == nullptr);
}

void test_integers_never_change_value_silently() {
    Bytes out;
    head(out, 5u, 4u);
    text(out, "big");
    head(out, 0u, (1ull << 53u));            /* exceeds exact double range */
    text(out, "max");
    head(out, 0u, (1ull << 53u) - 1u);       /* last exact integer */
    text(out, "neg");
    head(out, 1u, 41u);                      /* -42 */
    text(out, "hugeneg");
    head(out, 1u, 0xFFFFFFFFFFFFFFFEull);
    /* keys must be canonical: length-first then lexical */
    cJSON *record = decode(out);
    if (record == nullptr) {
        /* The strict parser may reject this key order; retry sorted. */
        Bytes sorted;
        head(sorted, 5u, 4u);
        text(sorted, "big");
        head(sorted, 0u, (1ull << 53u));
        text(sorted, "max");
        head(sorted, 0u, (1ull << 53u) - 1u);
        text(sorted, "neg");
        head(sorted, 1u, 41u);
        text(sorted, "hugeneg");
        head(sorted, 1u, 0xFFFFFFFFFFFFFFFEull);
        record = decode(sorted);
    }
    assert(record != nullptr);
    /* Beyond 2^53 the value is a decimal string, not a rounded double. */
    assert(std::strcmp(string_field(record, "big"), "9007199254740992") == 0);
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(record, "max");
    assert(cJSON_IsNumber(max) && max->valuedouble == 9007199254740991.0);
    const cJSON *neg = cJSON_GetObjectItemCaseSensitive(record, "neg");
    assert(cJSON_IsNumber(neg) && neg->valuedouble == -42.0);
    assert(string_field(record, "hugeneg") != nullptr);
    cJSON_Delete(record);
}

void test_unrepresentable_fields_are_skipped_not_fatal() {
    Bytes out;
    head(out, 5u, 3u);
    text(out, "cid");
    head(out, 2u, 4u); /* byte string */
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>(i));
    }
    text(out, "flag");
    out.push_back(0xf5); /* true */
    text(out, "text");
    text(out, "kept");
    cJSON *record = decode(out);
    assert(record != nullptr);
    assert(cJSON_GetObjectItemCaseSensitive(record, "cid") == nullptr);
    assert(std::strcmp(string_field(record, "text"), "kept") == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, "flag")));
    cJSON_Delete(record);
}

/* The pre-scan must not reject what real records contain: reply/embed
 * strongRefs carry a CID as tag 42 wrapping a byte string. */
void test_cid_links_and_nesting_from_real_records_still_decode() {
    Bytes canonical; /* canonical key order, as real records are */
    head(canonical, 5u, 3u);
    text(canonical, "text");
    text(canonical, "a reply");
    text(canonical, "$type");
    text(canonical, "app.bsky.feed.post");
    text(canonical, "reply");
    head(canonical, 5u, 2u);
    text(canonical, "root");
    head(canonical, 5u, 2u);
    text(canonical, "cid");
    head(canonical, 6u, 42u);
    /* 0x00 identity prefix + CIDv1 (dag-cbor, sha2-256, 32-byte digest). */
    head(canonical, 2u, 37u);
    canonical.insert(canonical.end(), {0x00, 0x01, 0x71, 0x12, 0x20});
    canonical.insert(canonical.end(), 32u, 0xab);
    text(canonical, "uri");
    text(canonical, "at://did:plc:x/app.bsky.feed.post/abc");
    text(canonical, "parent");
    head(canonical, 5u, 1u);
    text(canonical, "uri");
    text(canonical, "at://did:plc:x/app.bsky.feed.post/def");
    cJSON *decoded = decode(canonical);
    assert(decoded != nullptr);
    const cJSON *reply = cJSON_GetObjectItemCaseSensitive(decoded, "reply");
    const cJSON *root = cJSON_GetObjectItemCaseSensitive(reply, "root");
    assert(std::strcmp(string_field(root, "uri"), "at://did:plc:x/app.bsky.feed.post/abc") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(root, "cid") == nullptr); /* link skipped */
    cJSON_Delete(decoded);
}

Bytes nested_maps(std::size_t depth) {
    /* {"a": {"a": ... {"a": 0} ...}} — one-byte key "a" throughout. */
    Bytes out;
    for (std::size_t i = 0u; i < depth; ++i) {
        head(out, 5u, 1u);
        text(out, "a");
    }
    head(out, 0u, 0u);
    return out;
}

void test_depth_is_bounded() {
    /* Within the cap the nesting survives. */
    cJSON *shallow = decode(nested_maps(5u));
    assert(shallow != nullptr);
    int levels = 0;
    for (const cJSON *node = shallow; cJSON_IsObject(node);
         node = cJSON_GetObjectItemCaseSensitive(node, "a")) {
        ++levels;
    }
    assert(levels >= 5);
    cJSON_Delete(shallow);

    /* Past the cap the deep part is dropped, the record still decodes. */
    cJSON *deep = decode(nested_maps(200u));
    if (deep != nullptr) {
        int depth = 0;
        for (const cJSON *node = deep; cJSON_IsObject(node);
             node = cJSON_GetObjectItemCaseSensitive(node, "a")) {
            ++depth;
        }
        assert(depth <= 26);
        cJSON_Delete(deep);
    }

    /* Pathological nesting from an untrusted source must not overflow the
     * stack in the parser either. Whatever the answer, it must return. */
    for (const std::size_t depth : {2000u, 20000u, 200000u}) {
        cJSON *bomb = decode(nested_maps(depth));
        cJSON_Delete(bomb);
        Bytes arrays;
        for (std::size_t i = 0u; i < depth; ++i) {
            head(arrays, 4u, 1u);
        }
        head(arrays, 0u, 0u);
        Bytes wrapped;
        head(wrapped, 5u, 1u);
        text(wrapped, "a");
        wrapped.insert(wrapped.end(), arrays.begin(), arrays.end());
        cJSON_Delete(decode(wrapped));
    }
}

/* A few declared bytes must never buy unbounded work or memory: an item that
 * claims billions of children/bytes with almost no payload behind it has to be
 * rejected promptly, not allocated for. */
void test_huge_declared_lengths_are_rejected_promptly() {
    struct Case {
        const char *name;
        Bytes bytes;
    };
    const std::vector<Case> cases = {
        {"map count 2^16", {0xb9, 0xff, 0xff}},
        {"map count 2^24", {0xba, 0x00, 0xff, 0xff, 0xff}},
        {"map count 2^32-1", {0xba, 0xff, 0xff, 0xff, 0xff}},
        {"map count 2^40", {0xbb, 0, 0, 1, 0, 0, 0, 0, 0}},
        {"array count 2^32-1", {0xa1, 0x61, 'a', 0x9a, 0xff, 0xff, 0xff, 0xff}},
        {"string len 2^32-1", {0xa1, 0x61, 'a', 0x7a, 0xff, 0xff, 0xff, 0xff}},
        {"bytes len 2^32-1", {0xa1, 0x61, 'a', 0x5a, 0xff, 0xff, 0xff, 0xff}},
        {"string len 2^64-1", {0xa1, 0x61, 'a', 0x7b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
    };
    for (const Case &item : cases) {
        const auto start = std::chrono::steady_clock::now();
        cJSON *result = decode(item.bytes);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        cJSON_Delete(result);
        if (ms >= 500) {
            std::fprintf(stderr, "%s took %lld ms\n", item.name, static_cast<long long>(ms));
        }
        assert(ms < 500);
        assert(result == nullptr); /* none of these is a decodable record */
    }
}

/* Deterministic mutation sweep: flip, insert, delete and truncate bytes of a
 * valid record. Totality is the property; a decoded result must also print. */
void test_mutation_sweep_is_total() {
    const Bytes base = sample_post();
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&state]() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    };
    std::size_t decoded = 0u;
    for (int iteration = 0; iteration < 30000; ++iteration) {
        Bytes mutated = base;
        const unsigned edits = 1u + static_cast<unsigned>(next() % 4u);
        for (unsigned e = 0u; e < edits && !mutated.empty(); ++e) {
            const std::size_t at = static_cast<std::size_t>(next() % mutated.size());
            switch (next() % 4u) {
            case 0u:
                mutated[at] = static_cast<unsigned char>(next());
                break;
            case 1u:
                mutated.insert(mutated.begin() + static_cast<std::ptrdiff_t>(at),
                               static_cast<unsigned char>(next()));
                break;
            case 2u:
                mutated.erase(mutated.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            default:
                mutated.resize(at);
                break;
            }
        }
        cJSON *result = decode(mutated);
        if (result != nullptr) {
            ++decoded;
            char *printed = cJSON_PrintUnformatted(result);
            assert(printed != nullptr);
            std::free(printed);
            cJSON_Delete(result);
        }
    }
    assert(decoded > 0u); /* some mutations stay valid, so the sweep is meaningful */
}

} // namespace

int main() {
    test_decodes_a_realistic_record();
    test_rejects_non_records_and_bad_framing();
    test_integers_never_change_value_silently();
    test_unrepresentable_fields_are_skipped_not_fatal();
    test_cid_links_and_nesting_from_real_records_still_decode();
    test_depth_is_bounded();
    test_huge_declared_lengths_are_rejected_promptly();
    test_mutation_sweep_is_total();
    std::puts("cbor record tests passed");
    return 0;
}
