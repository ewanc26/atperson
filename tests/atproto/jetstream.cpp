/* Jetstream commit -> SyncObservation translation (#60).
 *
 * Every fixture is a real Jetstream `commit` JSON message (the flattened
 * JSON shape the firehose emits, not the CAR form). The extractor is pure:
 * parse, translate, assert — no network, no Wolfram. Covers create/update/
 * delete, other collections, malformed envelopes, empty text, replies and
 * quotes. */
#include "atproto/jetstream.hpp"
#include "atproto/extract.hpp"
#include "atproto/jetstream_filter.hpp"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cJSON.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

using atperson::PolicyReason;
using atperson::SyncObservation;

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

constexpr std::string_view SELF = "did:plc:self";
constexpr std::string_view AUTHOR = "did:plc:author";
constexpr std::string_view ROOT = "at://did:plc:root/app.bsky.feed.post/3k0";
constexpr std::string_view PARENT = "at://did:plc:parent/app.bsky.feed.post/3k1";
constexpr std::string_view QUOTED = "at://did:plc:quoted/app.bsky.feed.post/3k2";

void fail(const char *label) {
    std::fprintf(stderr, "FAIL %s\n", label);
    std::exit(1);
}

/* A bare Jetstream commit envelope around a record. `record_json` is spliced
 * in verbatim, so callers can build valid and malformed records alike. */
std::string commit(const std::string &operation, const std::string &collection,
                   const std::string &rkey, const std::string &record_json,
                   const std::string &did = std::string(AUTHOR)) {
    return std::string(R"({
        "did": ")") +
           did + R"(",
        "time_us": 1726200000123456,
        "kind": "commit",
        "commit": {
            "rev": "3kabcd",
            "operation": ")" +
           operation + R"(",
            "collection": ")" +
           collection + R"(",
            "rkey": ")" +
           rkey + R"(",
            "record": )" +
           record_json + R"(,
            "cid": "bafykzacaa..."
        }
    })";
}

std::string post(const std::string &text) {
    return R"({
        "$type": "app.bsky.feed.post",
        "text": ")" + text + R"(",
        "createdAt": "2026-09-17T00:00:00.000Z"
    })";
}

bool extract(const std::string &json, SyncObservation &out) {
    return atperson::extract_jetstream_commit(
        json.data(), json.size(), SELF, out);
}

std::filesystem::path filter_path(const char *name) {
    return std::filesystem::temp_directory_path() /
           (std::string("atperson-jetstream-filter-") + name + ".txt");
}

void write_filter(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        fail("could not create filter fixture");
    }
    output << text;
    output.close();
}

void test_collection_filter_file() {
    const auto path = filter_path("valid");
    write_filter(path,
                 "# explicit operator filter\n"
                 " app.bsky.feed.post \n"
                 "\n"
                 "app.bsky.graph.*\n"
                 "app.bsky.feed.post\n");

    const auto collections = atperson::load_jetstream_collections(path);
    if (collections.size() != 2u ||
        collections[0] != "app.bsky.feed.post" ||
        collections[1] != "app.bsky.graph.*") {
        fail("collection filter parse/order/dedup");
    }
    std::filesystem::remove(path);

    const auto defaults = atperson::default_jetstream_collections();
    if (!defaults.empty()) {
        fail("default collection filter");
    }
    std::printf("ok collection filter file\n");
}

void test_did_filter_file() {
    const auto path = filter_path("dids-valid");
    write_filter(path,
                 "# accounts to observe\n"
                 " did:plc:alpha \n"
                 "did:web:example.com\n"
                 "did:plc:alpha\n");

    const auto dids = atperson::load_jetstream_dids(path);
    if (dids.size() != 2u || dids[0] != "did:plc:alpha" ||
        dids[1] != "did:web:example.com") {
        fail("DID filter parse/order/dedup");
    }
    std::filesystem::remove(path);
    std::printf("ok DID filter file\n");
}

void test_did_filter_rejects_invalid_input() {
    {
        const auto path = filter_path("dids-invalid");
        write_filter(path, "not-a-did\n");
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_dids(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("non-DID wantedDids filter must reject");
        }
    }

    {
        const auto path = filter_path("dids-too-many");
        std::ofstream output(path, std::ios::trunc);
        for (std::size_t i = 0u; i < atperson::kJetstreamDidFilterLimit + 1u; ++i) {
            output << "did:plc:test" << i << '\n';
        }
        output.close();
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_dids(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("DID filter limit must reject");
        }
    }

    std::printf("ok invalid DID filters rejected\n");
}

void test_collection_filter_rejects_invalid_input() {
    {
        const auto path = filter_path("empty");
        write_filter(path, "# only comments\n\n");
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_collections(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("empty collection filter must reject");
        }
    }

    {
        const auto path = filter_path("whitespace");
        write_filter(path, "app.bsky.feed.post extra\n");
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_collections(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("collection filter internal whitespace must reject");
        }
    }

    {
        const auto path = filter_path("too-many");
        std::ofstream output(path, std::ios::trunc);
        for (std::size_t i = 0u;
             i < atperson::kJetstreamCollectionFilterLimit + 1u; ++i) {
            output << "example.collection." << i << '\n';
        }
        output.close();
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_collections(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("collection filter limit must reject");
        }
    }

    std::printf("ok invalid collection filters rejected\n");
}

void test_kind_filter_file() {
    const auto path = filter_path("kinds-valid");
    write_filter(path,
                 "# protocol evidence only\n"
                 " commit \n"
                 "commit\n"
                 "sync\n");

    const auto kinds = atperson::load_jetstream_kinds(path);
    if (kinds.size() != 2u || kinds[0] != "commit" || kinds[1] != "sync") {
        fail("kind filter parse/order/dedup");
    }
    std::filesystem::remove(path);
    std::printf("ok kind filter file\n");
}

void test_kind_filter_rejects_invalid_input() {
    {
        const auto path = filter_path("kinds-unknown");
        write_filter(path, "commit\nfirehose\n");
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_kinds(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("unknown event kind must reject");
        }
    }

    {
        const auto path = filter_path("kinds-too-many");
        /* The loader's unique-entry limit fires before kind validation,
         * so a fifth distinct (invalid) token exercises the bound. */
        write_filter(path, "commit\nidentity\naccount\nsync\ninfo\n");
        bool rejected = false;
        try {
            (void)atperson::load_jetstream_kinds(path);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        std::filesystem::remove(path);
        if (!rejected) {
            fail("kind filter limit must reject");
        }
    }

    std::printf("ok invalid kind filters rejected\n");
}

void test_create_top_level_post() {
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", post("hello public world")),
                 out)) {
        fail("create rejected a well-formed envelope");
    }
    if (out.text != "hello public world") {
        fail("create text");
    }
    if (out.source_uri != "at://did:plc:author/app.bsky.feed.post/3kcz9") {
        fail("create source uri");
    }
    if (out.author_did != AUTHOR) {
        fail("create author did");
    }
    if (out.created_at != "2026-09-17T00:00:00.000Z") {
        fail("create created-at");
    }
    if (!out.context.reply_root_uri.empty() || !out.context.reply_parent_uri.empty() ||
        !out.context.quote_uri.empty()) {
        fail("create must carry no conversation context");
    }
    if (out.policy_reason != PolicyReason::Eligible) {
        fail("create reason");
    }
    std::printf("ok create top-level post\n");
}

void test_self_authored_post_is_skipped() {
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "self1",
                        post("do not learn me"), std::string(SELF)), out)) {
        fail("self-authored post rejected instead of ledgered");
    }
    if (!out.text.empty() || out.policy_reason != PolicyReason::SelfAuthored) {
        fail("self-authored policy");
    }
    std::printf("ok self-authored Jetstream post skipped\n");
}

void test_update_post() {
    SyncObservation out;
    if (!extract(commit("update", "app.bsky.feed.post", "3kcz9", post("edited text")), out)) {
        fail("update rejected a well-formed envelope");
    }
    if (out.text != "edited text") {
        fail("update text");
    }
    std::printf("ok update post\n");
}

void test_delete_is_not_an_observation() {
    SyncObservation out;
    if (extract(commit("delete", "app.bsky.feed.post", "3kcz9", "null"), out)) {
        fail("delete must not extract");
    }
    std::printf("ok delete skipped\n");
}

void test_other_collection_is_not_an_observation() {
    SyncObservation out;
    if (extract(commit("create", "app.bsky.feed.like", "3kcz9", R"({"subject":{}})"), out)) {
        fail("non-post collection must not extract");
    }
    std::printf("ok non-post collection skipped\n");
}

void test_malformed_json_is_rejected() {
    SyncObservation out;
    if (extract("{\"did\": \"did:plc:author\", \"commit\":", out)) {
        fail("malformed JSON must not extract");
    }
    if (extract(std::string(), out)) {
        fail("empty input must not extract");
    }
    std::printf("ok malformed JSON rejected\n");
}

void test_envelope_without_did_is_rejected() {
    SyncObservation out;
    if (extract(commit("create", "app.bsky.feed.post", "3kcz9", post("x"), ""), out)) {
        fail("missing did must not extract");
    }
    std::printf("ok missing did rejected\n");
}

void test_empty_text_is_skipped_with_reason() {
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", post("")), out)) {
        fail("empty-text post must still extract for the ledger");
    }
    if (!out.text.empty() || out.policy_reason != PolicyReason::EmptyText) {
        fail("empty-text reason");
    }
    std::printf("ok empty text carries EmptyText reason\n");
}

void test_reply_context_preserved() {
    std::string record = R"({
        "$type": "app.bsky.feed.post",
        "text": "a reply",
        "createdAt": "2026-09-17T00:00:00.000Z",
        "reply": {
            "root": {"uri": "at://did:plc:root/app.bsky.feed.post/3k0", "cid": "bafy0"},
            "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k1", "cid": "bafy1"}
        }
    })";
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", record), out)) {
        fail("reply rejected");
    }
    if (out.context.reply_root_uri != ROOT || out.context.reply_parent_uri != PARENT) {
        fail("reply root/parent URIs");
    }
    if (!out.context.quote_uri.empty()) {
        fail("reply should carry no quote");
    }
    if (out.policy_reason != PolicyReason::Reply) {
        fail("reply reason");
    }
    std::printf("ok reply context preserved\n");
}

void test_malformed_reply_context_is_graceful() {
    std::string record = R"({
        "$type": "app.bsky.feed.post",
        "text": "reply-ish",
        "createdAt": "2026-09-17T00:00:00.000Z",
        "reply": {"root": "not-an-object", "parent": null}
    })";
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", record), out)) {
        fail("malformed reply must not drop the item");
    }
    if (out.text != "reply-ish" || !out.context.reply_root_uri.empty() ||
        !out.context.reply_parent_uri.empty()) {
        fail("malformed reply must yield empty context, kept item");
    }
    std::printf("ok malformed reply context graceful\n");
}

void test_quote_context_preserved() {
    std::string record = R"({
        "$type": "app.bsky.feed.post",
        "text": "quote with own words",
        "createdAt": "2026-09-17T00:00:00.000Z",
        "embed": {
            "$type": "app.bsky.embed.record",
            "record": {"uri": "at://did:plc:quoted/app.bsky.feed.post/3k2", "cid": "bafy2"}
        }
    })";
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", record), out)) {
        fail("quote rejected");
    }
    if (out.context.quote_uri != QUOTED) {
        fail("quote uri");
    }
    if (out.policy_reason != PolicyReason::Quote) {
        fail("quote reason");
    }
    std::printf("ok quote context preserved\n");
}

void test_non_record_embed_is_ignored() {
    std::string record = R"({
        "$type": "app.bsky.feed.post",
        "text": "image post",
        "createdAt": "2026-09-17T00:00:00.000Z",
        "embed": {"$type": "app.bsky.embed.images", "images": []}
    })";
    SyncObservation out;
    if (!extract(commit("create", "app.bsky.feed.post", "3kcz9", record), out)) {
        fail("image post rejected");
    }
    if (!out.context.quote_uri.empty() || out.text != "image post") {
        fail("non-record embed must not yield a quote");
    }
    std::printf("ok non-record embed ignored\n");
}

void test_normalized_v2_commit_is_extractable() {
    const std::string normalized = R"({
        "did": "did:plc:author",
        "cursor": 12345,
        "kind": "commit",
        "commit": {
            "operation": "create",
            "collection": "app.bsky.feed.post",
            "rkey": "3knormalized",
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "normalized v2 post",
                "createdAt": "2026-09-20T00:00:00.000Z"
            }
        }
    })";
    SyncObservation out;
    if (!extract(normalized, out) || out.text != "normalized v2 post") {
        fail("normalized v2 commit extraction");
    }
    std::printf("ok normalized v2 commit extracted\n");
}

void test_polling_and_jetstream_converge() {
    const std::string record = post("same public observation");
    SyncObservation js;
    if (!extract(commit("create", "app.bsky.feed.post", "same1", record), js)) {
        fail("Jetstream parity fixture");
    }

    const std::string feed_json = R"({
        "post": {
            "uri": "at://did:plc:author/app.bsky.feed.post/same1",
            "author": {"did": "did:plc:author"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "same public observation",
                "createdAt": "2026-09-17T00:00:00.000Z"
            },
            "viewer": {}
        }
    })";
    Json item(cJSON_Parse(feed_json.c_str()));
    if (!item) {
        fail("polling parity fixture parse");
    }
    SyncObservation polling;
    if (!atperson::extract_feed_item(item.get(), SELF, polling)) {
        fail("polling parity fixture extraction");
    }

    if (js.text != polling.text || js.source_uri != polling.source_uri ||
        js.author_did != polling.author_did ||
        js.created_at != polling.created_at ||
        js.policy_reason != polling.policy_reason ||
        js.context.reply_root_uri != polling.context.reply_root_uri ||
        js.context.reply_parent_uri != polling.context.reply_parent_uri ||
        js.context.quote_uri != polling.context.quote_uri) {
        fail("polling/Jetstream distilled observation mismatch");
    }

    const auto root = std::filesystem::temp_directory_path() /
                      "atperson-jetstream-parity";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        atperson::LanguageGraph graph_a;
        atperson::LanguageGraph graph_b;
        atperson::Ledger ledger_a(root / "polling.bin");
        atperson::Ledger ledger_b(root / "jetstream.bin");

        if (!atperson::process_observation(graph_a, ledger_a, polling) ||
            !atperson::process_observation(graph_b, ledger_b, js)) {
            fail("parity fixture unexpectedly deduplicated");
        }

        const auto entries_a = ledger_a.entries();
        const auto entries_b = ledger_b.entries();
        if (entries_a.size() != 1u || entries_b.size() != 1u) {
            fail("parity ledger count");
        }
        const auto &a = entries_a.front();
        const auto &b = entries_b.front();
        if (a.observed_at != b.observed_at ||
            a.content_digest != b.content_digest ||
            a.schema_version != b.schema_version ||
            a.outcome != b.outcome ||
            std::string(a.source_id) != std::string(b.source_id) ||
            std::string(a.author_did) != std::string(b.author_did) ||
            ledger_a.payload(a.id) != ledger_b.payload(b.id)) {
            fail("polling/Jetstream ledger mismatch");
        }

        const auto stats_a = graph_a.stats();
        const auto stats_b = graph_b.stats();
        if (stats_a.node_count != stats_b.node_count ||
            stats_a.edge_count != stats_b.edge_count ||
            stats_a.training_steps != stats_b.training_steps ||
            stats_a.observations != stats_b.observations) {
            fail("polling/Jetstream graph mismatch");
        }
    }

    std::filesystem::remove_all(root);
    std::printf("ok polling and Jetstream converge\n");
}

void test_record_not_an_object_is_rejected() {
    SyncObservation out;
    if (extract(commit("create", "app.bsky.feed.post", "3kcz9", R"("just a string")"), out)) {
        fail("string record must not extract");
    }
    std::printf("ok non-object record rejected\n");
}

} // namespace

int main() {
    test_collection_filter_file();
    test_collection_filter_rejects_invalid_input();
    test_did_filter_file();
    test_did_filter_rejects_invalid_input();
    test_kind_filter_file();
    test_kind_filter_rejects_invalid_input();
    test_create_top_level_post();
    test_self_authored_post_is_skipped();
    test_update_post();
    test_delete_is_not_an_observation();
    test_other_collection_is_not_an_observation();
    test_malformed_json_is_rejected();
    test_envelope_without_did_is_rejected();
    test_empty_text_is_skipped_with_reason();
    test_reply_context_preserved();
    test_malformed_reply_context_is_graceful();
    test_quote_context_preserved();
    test_non_record_embed_is_ignored();
    test_normalized_v2_commit_is_extractable();
    test_polling_and_jetstream_converge();
    test_record_not_an_object_is_rejected();

    std::printf("jetstream extract tests passed\n");
    return 0;
}
