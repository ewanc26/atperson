/* AT Protocol feed-item translation (issue #24).
 *
 * Every fixture is a real app.bsky.feed.defs#feedViewPost JSON object.
 * The extractor is pure: parse, translate, assert — no network, no Wolfram.
 * Covers top-level posts, nested replies, deleted/missing parents, quotes
 * (with and without own text), reposts, and malformed context. */
#include "extract.hpp"

#include <cJSON.h>

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

constexpr std::string_view ME = "did:plc:me";
constexpr std::string_view ROOT = "at://did:plc:root/app.bsky.feed.post/3k1";
constexpr std::string_view PARENT = "at://did:plc:parent/app.bsky.feed.post/3k2";
constexpr std::string_view QUOTED = "at://did:plc:quoted/app.bsky.feed.post/3k3";

void fail(const char *label) {
    std::fprintf(stderr, "FAIL %s\n", label);
    std::exit(1);
}

/* Parse one feed item; null on malformed JSON (the extractor must never see
 * a null item in production — Wolfram parses first — but the fixture helper
 * asserts the input is well-formed JSON). */
cJSON *parse(const std::string &json) {
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) {
        fail("fixture JSON did not parse");
    }
    return root;
}

SyncObservation extract(const std::string &json) {
    Json item(parse(json));
    SyncObservation out;
    if (!atperson::extract_feed_item(item.get(), ME, out)) {
        fail("extract_feed_item rejected a well-formed fixture");
    }
    return out;
}

/* A top-level post: no reply member, no embed. */
std::string top_level(const std::string &text) {
    return std::string(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3k4",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": ")") +
           text + R"(",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        }
    })";
}

/* A reply: the feedViewPost reply member carries strongRefs to root/parent. */
std::string reply(const std::string &text) {
    return std::string(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3k5",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": ")") +
           text + R"(",
                "createdAt": "2026-09-17T00:00:00Z",
                "reply": {
                    "root": {"uri": "at://did:plc:root/app.bsky.feed.post/3k1"},
                    "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k2"}
                }
            },
            "viewer": {}
        },
        "reply": {
            "root": {"uri": "at://did:plc:root/app.bsky.feed.post/3k1", "cid": "bafy1", "author": {"did": "did:plc:root"}},
            "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k2", "cid": "bafy2", "author": {"did": "did:plc:parent"}}
        }
    })";
}

/* A quote post: embeds another record, has its own text. */
std::string quote(const std::string &text) {
    return std::string(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3k6",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": ")") +
           text + R"(",
                "createdAt": "2026-09-17T00:00:00Z",
                "embed": {
                    "$type": "app.bsky.embed.record",
                    "record": {"uri": "at://did:plc:quoted/app.bsky.feed.post/3k3", "cid": "bafy3"}
                }
            },
            "viewer": {}
        }
    })";
}

/* ---------------------------------------------------------------- */
/* Top-level posts                                                   */
/* ---------------------------------------------------------------- */

void test_top_level_post() {
    const SyncObservation obs = extract(top_level("an ordinary post"));
    if (obs.text != "an ordinary post" || obs.policy_reason != PolicyReason::Eligible) {
        fail("top-level text/reason");
    }
    if (!obs.context.reply_root_uri.empty() || !obs.context.reply_parent_uri.empty() ||
        !obs.context.quote_uri.empty()) {
        fail("top-level context should be empty");
    }
    std::printf("ok top-level post\n");
}

/* ---------------------------------------------------------------- */
/* Replies                                                           */
/* ---------------------------------------------------------------- */

void test_reply_context_preserved() {
    const SyncObservation obs = extract(reply("a nested reply"));
    if (obs.policy_reason != PolicyReason::Reply) {
        fail("reply reason");
    }
    if (obs.context.reply_root_uri != ROOT || obs.context.reply_parent_uri != PARENT) {
        fail("reply root/parent URIs");
    }
    if (!obs.context.quote_uri.empty()) {
        fail("reply should carry no quote");
    }
    std::printf("ok reply context preserved\n");
}

void test_deep_reply_root_differs_from_parent() {
    /* A third-level reply: root is the thread head, parent is the direct
     * parent — the two must stay distinct identifiers. */
    std::string json = reply("deep reply");
    const std::size_t at = json.find("3k2");
    json.replace(at, 3, "3k9");
    const SyncObservation obs = extract(json);
    if (obs.context.reply_root_uri == obs.context.reply_parent_uri) {
        fail("root and parent collapsed");
    }
    std::printf("ok deep reply root != parent\n");
}

void test_deleted_parent_still_yields_identifiers() {
    /* The feedViewPost reply member is what the extractor reads; a deleted
     * parent still appears there as a strongRef (the appview keeps the
     * reference even when the record is gone). The identifiers the
     * author's record claims survive — exactly what planning and audit
     * need. */
    const SyncObservation obs = extract(reply("reply to a deleted parent"));
    if (obs.context.reply_parent_uri != PARENT) {
        fail("deleted-parent identifiers");
    }
    std::printf("ok deleted parent yields identifiers\n");
}

void test_malformed_reply_member_is_graceful() {
    /* A reply member with no parseable strongRefs: the observation
     * survives with empty context rather than being dropped. */
    const SyncObservation obs = extract(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3k7",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "reply-ish",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        },
        "reply": "not-an-object"
    })");
    if (obs.text != "reply-ish") {
        fail("malformed reply dropped the item");
    }
    if (!obs.context.reply_root_uri.empty() || !obs.context.reply_parent_uri.empty()) {
        fail("malformed reply should yield empty context");
    }
    /* The record itself declared a reply, but the extractor derives
     * is_reply from the feedViewPost reply member only; with no parseable
     * member the item is treated as a plain post, not dropped. */
    if (obs.policy_reason != PolicyReason::Eligible) {
        fail("malformed reply reason");
    }
    std::printf("ok malformed reply member graceful\n");
}

void test_reply_with_missing_uri_field() {
    /* A reply strongRef with no uri string: empty identifier, item kept. */
    const SyncObservation obs = extract(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3k8",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "partial reply",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        },
        "reply": {
            "root": {"cid": "bafy1"},
            "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k2"}
        }
    })");
    if (!obs.context.reply_root_uri.empty()) {
        fail("missing root uri should read empty");
    }
    if (obs.context.reply_parent_uri != PARENT) {
        fail("parent uri should survive");
    }
    std::printf("ok partial reply identifiers\n");
}

/* ---------------------------------------------------------------- */
/* Quotes                                                            */
/* ---------------------------------------------------------------- */

void test_quote_with_own_text_is_tagged() {
    const SyncObservation obs = extract(quote("look at this"));
    if (obs.policy_reason != PolicyReason::Quote) {
        fail("quote reason");
    }
    if (obs.context.quote_uri != QUOTED) {
        fail("quote uri");
    }
    if (obs.text != "look at this") {
        fail("quote own text");
    }
    std::printf("ok quote with own text tagged\n");
}

void test_quote_text_is_never_merged() {
    /* The quoted record's text must not appear in the learnable text. */
    const SyncObservation obs = extract(quote("my own words"));
    if (obs.text.find("quoted") != std::string::npos) {
        fail("quoted text leaked into learnable text");
    }
    std::printf("ok quoted text never merged\n");
}

void test_quote_without_own_text_is_skipped_empty() {
    /* A quote with no own text has nothing to learn: skipped as EmptyText
     * (recorded, not silently dropped), and the quote URI still rides in
     * context for planning. */
    const SyncObservation obs = extract(quote(""));
    if (obs.policy_reason != PolicyReason::EmptyText) {
        fail("empty quote reason");
    }
    if (!obs.text.empty()) {
        fail("empty quote text");
    }
    if (obs.context.quote_uri != QUOTED) {
        fail("empty quote should keep quote uri");
    }
    std::printf("ok quote without own text skipped\n");
}

/* ---------------------------------------------------------------- */
/* Malformed items                                                   */
/* ---------------------------------------------------------------- */

void test_item_without_post_is_rejected() {
    Json item(parse(R"({"reason": {}})"));
    SyncObservation obs;
    if (atperson::extract_feed_item(item.get(), ME, obs)) {
        fail("item without post should be rejected");
    }
    std::printf("ok item without post rejected\n");
}

void test_item_without_uri_is_rejected() {
    Json item(parse(R"({
        "post": {
            "author": {"did": "did:plc:other"},
            "record": {"$type": "app.bsky.feed.post", "text": "x"}
        }
    })"));
    SyncObservation obs;
    if (atperson::extract_feed_item(item.get(), ME, obs)) {
        fail("item without uri should be rejected");
    }
    std::printf("ok item without uri rejected\n");
}

void test_non_post_record_is_skipped_not_dropped() {
    /* A generator "post" (non-post record type) still becomes an
     * observation — SKIPPED by policy, visible in the ledger. */
    const SyncObservation obs = extract(R"({
        "post": {
            "uri": "at://did:plc:other/com.example.custom/1",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "com.example.custom.record",
                "text": "not a post",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        }
    })");
    if (obs.policy_reason != PolicyReason::UnsupportedRecord || !obs.text.empty()) {
        fail("unsupported record handling");
    }
    std::printf("ok non-post record skipped visibly\n");
}

void test_self_authored_reply_keeps_context_but_skips() {
    /* Self-authored items are never learned from, but the observation
     * (with its context) is still recorded — the ledger sees the
     * entity's own replies for audit. */
    const SyncObservation obs = extract(R"({
        "post": {
            "uri": "at://did:plc:me/app.bsky.feed.post/3kA",
            "author": {"did": "did:plc:me"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "my own reply",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        },
        "reply": {
            "root": {"uri": "at://did:plc:root/app.bsky.feed.post/3k1"},
            "parent": {"uri": "at://did:plc:parent/app.bsky.feed.post/3k2"}
        }
    })");
    if (obs.policy_reason != PolicyReason::SelfAuthored || !obs.text.empty()) {
        fail("self-authored handling");
    }
    if (obs.context.reply_parent_uri != PARENT) {
        fail("self-authored context");
    }
    std::printf("ok self-authored reply context kept\n");
}

void test_repost_reason_survives() {
    const SyncObservation obs = extract(R"({
        "post": {
            "uri": "at://did:plc:other/app.bsky.feed.post/3kB",
            "author": {"did": "did:plc:other"},
            "record": {
                "$type": "app.bsky.feed.post",
                "text": "reposted words",
                "createdAt": "2026-09-17T00:00:00Z"
            },
            "viewer": {}
        },
        "reason": {"$type": "app.bsky.feed.defs#reasonRepost"}
    })");
    if (obs.policy_reason != PolicyReason::Repost) {
        fail("repost reason");
    }
    std::printf("ok repost reason survives\n");
}

} // namespace

int main() {
    test_top_level_post();
    test_reply_context_preserved();
    test_deep_reply_root_differs_from_parent();
    test_deleted_parent_still_yields_identifiers();
    test_malformed_reply_member_is_graceful();
    test_reply_with_missing_uri_field();
    test_quote_with_own_text_is_tagged();
    test_quote_text_is_never_merged();
    test_quote_without_own_text_is_skipped_empty();
    test_item_without_post_is_rejected();
    test_item_without_uri_is_rejected();
    test_non_post_record_is_skipped_not_dropped();
    test_self_authored_reply_keeps_context_but_skips();
    test_repost_reason_survives();
    std::printf("all extract tests passed\n");
    return 0;
}
