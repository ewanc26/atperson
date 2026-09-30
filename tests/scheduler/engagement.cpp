/* Engagement consent: the rules, the target of each action kind, the invited
 * set and the do-not-engage file. Pure and offline. */

#include "scheduler/engagement.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

const std::set<std::string> kInvited = {"did:plc:friend"};

DoNotEngage list_of(std::initializer_list<const char *> dids) {
    DoNotEngage list;
    for (const char *did : dids) {
        list.dids.insert(did);
    }
    return list;
}

EngagementRefusal check(OutboundActionKind kind, const char *target, EngagementMode mode,
                        const DoNotEngage &opted_out = {}) {
    EngagementConfig config;
    config.mode = mode;
    return check_engagement(kind, target, config, kInvited, opted_out);
}

void test_invited_mode_requires_engagement_for_likes_reposts_and_follows() {
    for (const auto kind : {OutboundActionKind::Like, OutboundActionKind::Repost,
                            OutboundActionKind::Follow}) {
        assert(check(kind, "did:plc:friend", EngagementMode::Invited) == EngagementRefusal::None);
        assert(check(kind, "did:plc:stranger", EngagementMode::Invited) ==
               EngagementRefusal::NotInvited);
        /* Open mode drops only the invitation requirement. */
        assert(check(kind, "did:plc:stranger", EngagementMode::Open) == EngagementRefusal::None);
    }
}

void test_replies_and_posts_are_not_gated_by_invitation() {
    /* A reply continues a conversation the entity started: already an invitation. */
    assert(check(OutboundActionKind::Reply, "did:plc:stranger", EngagementMode::Invited) ==
           EngagementRefusal::None);
    /* A post is directed at nobody. */
    assert(check(OutboundActionKind::Post, "", EngagementMode::Invited) == EngagementRefusal::None);
    /* No target at all is never a refusal, even for a like (nothing to check). */
    assert(check(OutboundActionKind::Like, "", EngagementMode::Invited) == EngagementRefusal::None);
}

void test_the_opt_out_list_beats_everything() {
    const DoNotEngage list = list_of({"did:plc:friend", "did:plc:stranger"});
    for (const auto mode : {EngagementMode::Invited, EngagementMode::Open}) {
        for (const auto kind : {OutboundActionKind::Like, OutboundActionKind::Repost,
                                OutboundActionKind::Follow, OutboundActionKind::Reply}) {
            /* Even someone who engaged with us first, and even for a reply. */
            assert(check(kind, "did:plc:friend", mode, list) == EngagementRefusal::OptedOut);
            assert(check(kind, "did:plc:stranger", mode, list) == EngagementRefusal::OptedOut);
        }
    }
    /* Someone not on the list is unaffected. */
    assert(check(OutboundActionKind::Reply, "did:plc:other", EngagementMode::Open, list) ==
           EngagementRefusal::None);
    assert(std::string(engagement_refusal_name(EngagementRefusal::OptedOut)) == "opted_out");
    assert(std::string(engagement_refusal_name(EngagementRefusal::NotInvited)) == "not_invited");
    assert(std::string(engagement_mode_name(EngagementMode::Invited)) == "invited");
}

void test_the_target_of_each_action() {
    OutboundAction reply;
    reply.kind = OutboundActionKind::Reply;
    reply.reply_root = "at://did:plc:root/app.bsky.feed.post/1";
    reply.reply_parent = "at://did:plc:parent/app.bsky.feed.post/2";
    assert(action_target_did(reply) == "did:plc:parent");

    OutboundAction like;
    like.kind = OutboundActionKind::Like;
    like.subject = "at://did:plc:author/app.bsky.feed.post/3kabc";
    assert(action_target_did(like) == "did:plc:author");
    like.kind = OutboundActionKind::Repost;
    assert(action_target_did(like) == "did:plc:author");

    OutboundAction follow;
    follow.kind = OutboundActionKind::Follow;
    follow.subject = "did:plc:person";
    assert(action_target_did(follow) == "did:plc:person");

    OutboundAction post;
    post.kind = OutboundActionKind::Post;
    post.text = "hello";
    assert(action_target_did(post).empty());

    /* A malformed subject yields no DID rather than a guess. */
    like.kind = OutboundActionKind::Like;
    like.subject = "not-an-at-uri";
    assert(action_target_did(like).empty());
    like.subject = "at://did:plc:author"; /* authority only */
    assert(action_target_did(like) == "did:plc:author");
}

void test_invited_authors_come_from_journal_events() {
    JournalContents journal;
    journal.events.push_back({"a1", "at://x/1", "did:plc:one", "parent", "t"});
    journal.events.push_back({"a1", "at://x/2", "did:plc:one", "quote", "t"});
    journal.events.push_back({"a2", "at://x/3", "did:plc:two", "root", "t"});
    journal.events.push_back({"a2", "at://x/4", "", "root", "t"}); /* no author: ignored */
    assert((invited_authors(journal) == std::set<std::string>{"did:plc:one", "did:plc:two"}));
    assert(invited_authors(JournalContents{}).empty());
}

void test_do_not_engage_file() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("atperson-engagement-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "dne.txt";
    assert(load_do_not_engage(file).dids.empty()); /* missing file: nobody opted out */
    {
        std::ofstream out(file);
        out << "# people who asked not to be contacted\n"
               "did:plc:alpha\n"
               "  did:web:example.com  # trailing comment\n"
               "\n"
               "not a did\n"
               "alice.bsky.social\n"
               "did:plc:alpha\n";
    }
    const DoNotEngage list = load_do_not_engage(file);
    assert((list.dids == std::set<std::string>{"did:plc:alpha", "did:web:example.com"}));
    assert(list.invalid_lines == 2u); /* the two non-DID lines, counted not fatal */
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_invited_mode_requires_engagement_for_likes_reposts_and_follows();
    test_replies_and_posts_are_not_gated_by_invitation();
    test_the_opt_out_list_beats_everything();
    test_the_target_of_each_action();
    test_invited_authors_come_from_journal_events();
    test_do_not_engage_file();
    std::puts("engagement tests passed");
    return 0;
}
