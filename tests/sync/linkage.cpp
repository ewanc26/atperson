/* Action-event linkage tests (#27): public replies and quotes associate
 * with executed outbound actions by stable at-URI, never by text. Covers
 * parent/root/quote linkage, first-field-wins for double references,
 * duplicate suppression across restarts, and the full sync-engine path with
 * a fixture linker. Offline: the journal is a scratch file. */

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "engine.hpp"
#include "ingestion/state.hpp"
#include "journal/store.hpp"
#include "linkage.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-linkage-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

constexpr const char *ACTION_URI = "at://did:plc:self/app.bsky.feed.post/3kabc";

/* An executed post in the journal, as publish would have recorded it. */
void seed_executed_action(const std::filesystem::path &journal_path) {
    atperson::JournalAction action;
    action.id = "3kabc";
    action.kind = "post";
    action.text = "the exact approved text";
    action.digest = "0123456789abcdef";
    action.outcome = atperson::JournalActionOutcome::Executed;
    action.reason = "allow";
    action.uri = ACTION_URI;
    action.cid = "bafyreiexample";
    action.at = "2026-09-16T00:00:00Z";
    atperson::append_journal_action(journal_path, action);
}

atperson::ConversationContext reply_context(const std::string &parent, const std::string &root) {
    atperson::ConversationContext context;
    context.reply_parent_uri = parent;
    context.reply_root_uri = root;
    return context;
}

void test_links_direct_reply_by_parent_uri() {
    const auto dir = scratch_dir("parent");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    const auto result = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/reply1", "did:plc:other",
        reply_context(ACTION_URI, "at://did:plc:threadroot/app.bsky.feed.post/root1"),
        "2026-09-16T01:00:00Z");

    assert(result.linked == 1u);
    assert(result.duplicates == 0u);
    const auto reloaded = atperson::load_journal(journal_path);
    assert(reloaded.events.size() == 1u);
    assert(reloaded.events[0].action_id == "3kabc");
    assert(reloaded.events[0].event_uri == "at://did:plc:other/app.bsky.feed.post/reply1");
    assert(reloaded.events[0].author_did == "did:plc:other");
    assert(reloaded.events[0].via == "parent");
}

void test_links_thread_reply_by_root_uri() {
    const auto dir = scratch_dir("root");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    /* The reply's parent is another reply, but the thread root is the
     * executed action: the root field still associates it. */
    const auto result = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/reply2", "did:plc:other",
        reply_context("at://did:plc:other/app.bsky.feed.post/reply1", ACTION_URI),
        "2026-09-16T02:00:00Z");

    assert(result.linked == 1u);
    const auto reloaded = atperson::load_journal(journal_path);
    assert(reloaded.events.size() == 1u);
    assert(reloaded.events[0].via == "root");
}

void test_links_quote_by_quote_uri() {
    const auto dir = scratch_dir("quote");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    atperson::ConversationContext context;
    context.quote_uri = ACTION_URI;
    const auto result = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/quote1", "did:plc:other",
        context, "2026-09-16T03:00:00Z");

    assert(result.linked == 1u);
    const auto reloaded = atperson::load_journal(journal_path);
    assert(reloaded.events.size() == 1u);
    assert(reloaded.events[0].via == "quote");
}

void test_first_field_wins_for_double_reference() {
    const auto dir = scratch_dir("double");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    /* A reply whose parent and root are both the executed action (a
     * self-reply thread) links once, via the parent field. */
    const auto result = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/reply3", "did:plc:other",
        reply_context(ACTION_URI, ACTION_URI), "2026-09-16T04:00:00Z");

    assert(result.linked == 1u);
    const auto reloaded = atperson::load_journal(journal_path);
    assert(reloaded.events.size() == 1u);
    assert(reloaded.events[0].via == "parent");
}

void test_unrelated_observations_link_nothing() {
    const auto dir = scratch_dir("unrelated");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    const auto result = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/other", "did:plc:other",
        reply_context("at://did:plc:elsewhere/app.bsky.feed.post/x",
                      "at://did:plc:elsewhere/app.bsky.feed.post/y"),
        "2026-09-16T05:00:00Z");

    assert(result.linked == 0u);
    assert(result.duplicates == 0u);
    const auto reloaded = atperson::load_journal(journal_path);
    assert(reloaded.events.empty());
}

void test_duplicate_events_link_once() {
    const auto dir = scratch_dir("duplicate");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);
    auto journal = atperson::load_journal(journal_path);

    const auto context = reply_context(ACTION_URI, ACTION_URI);
    const auto first = atperson::link_observation(
        journal_path, journal, "at://did:plc:other/app.bsky.feed.post/reply1", "did:plc:other",
        context, "2026-09-16T06:00:00Z");
    assert(first.linked == 1u);

    /* Restart: reload the journal from disk and re-ingest the same page.
     * The event is already recorded; it links zero times. */
    auto reloaded_journal = atperson::load_journal(journal_path);
    const auto second = atperson::link_observation(
        journal_path, reloaded_journal, "at://did:plc:other/app.bsky.feed.post/reply1",
        "did:plc:other", context, "2026-09-16T06:00:00Z");
    assert(second.linked == 0u);
    assert(second.duplicates == 1u);

    const auto final_journal = atperson::load_journal(journal_path);
    assert(final_journal.events.size() == 1u);
}

/* ---------------------------------------------------------------- */
/* Sync-engine integration                                           */
/* ---------------------------------------------------------------- */

atperson::SyncObservation reply_observation(const std::string &uri, const std::string &parent) {
    atperson::SyncObservation observation;
    observation.text = "a reply to the action";
    observation.source_uri = uri;
    observation.author_did = "did:plc:other";
    observation.created_at = "2026-09-16T07:00:00Z";
    observation.context = reply_context(parent, parent);
    return observation;
}

void test_run_sync_links_events_during_traversal() {
    const auto dir = scratch_dir("engine");
    const auto journal_path = dir / "action-journal.jsonl";
    seed_executed_action(journal_path);

    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    const auto page = []() {
        atperson::SyncPage fixture;
        fixture.items.push_back(
            reply_observation("at://did:plc:other/app.bsky.feed.post/reply1", ACTION_URI));
        fixture.next_cursor = std::nullopt;
        return fixture;
    };
    const auto fetch_page = [&page](const std::optional<std::string> &) { return page(); };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    const auto link = atperson::make_journal_linker(journal_path);
    const auto result = atperson::run_sync(graph, ledger, state, fetch_page, limits, link);
    assert(result.learned == 1u);

    const auto journal = atperson::load_journal(journal_path);
    assert(journal.events.size() == 1u);
    assert(journal.events[0].action_id == "3kabc");
    assert(journal.events[0].via == "parent");

    /* Re-ingest the same page (fresh traversal): the ledger dedups the
     * observation and the journal dedups the event. */
    auto state2 = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");
    const auto result2 = atperson::run_sync(graph, ledger, state2, fetch_page, limits,
                                            atperson::make_journal_linker(journal_path));
    assert(result2.learned == 0u);
    assert(result2.duplicates == 1u);
    const auto journal2 = atperson::load_journal(journal_path);
    assert(journal2.events.size() == 1u);
}

void test_missing_journal_disables_linkage() {
    const auto dir = scratch_dir("missing");
    atperson::LanguageGraph graph;
    atperson::Ledger ledger(dir / "ledger.bin");
    auto state = atperson::initial_ingestion_state("https://bsky.social", "did:plc:abc");

    const auto page = []() {
        atperson::SyncPage fixture;
        fixture.items.push_back(
            reply_observation("at://did:plc:other/app.bsky.feed.post/reply1", ACTION_URI));
        fixture.next_cursor = std::nullopt;
        return fixture;
    };
    const auto fetch_page = [&page](const std::optional<std::string> &) { return page(); };

    atperson::SyncLimits limits;
    limits.max_pages = 1;
    /* No journal exists: the linker is null and sync proceeds normally. */
    const auto link = atperson::make_journal_linker(dir / "absent-journal.jsonl");
    assert(!link);
    const auto result = atperson::run_sync(graph, ledger, state, fetch_page, limits, link);
    assert(result.learned == 1u);
    assert(!std::filesystem::exists(dir / "absent-journal.jsonl"));
}

} // namespace

int main() {
    test_links_direct_reply_by_parent_uri();
    test_links_thread_reply_by_root_uri();
    test_links_quote_by_quote_uri();
    test_first_field_wins_for_double_reference();
    test_unrelated_observations_link_nothing();
    test_duplicate_events_link_once();
    test_run_sync_links_events_during_traversal();
    test_missing_journal_disables_linkage();
    std::printf("all action-event linkage tests passed\n");
    return 0;
}
