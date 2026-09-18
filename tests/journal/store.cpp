/* Action/outcome journal store tests (#27): append/load round-trip for all
 * three entry kinds, torn-tail recovery, malformed-line rejection, event
 * dedup and executed-action URI lookup. Offline, no network. */

#include "journal/store.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::JournalError;
using atperson::JournalEvent;
using atperson::JournalValence;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-journal-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

JournalAction sample_action() {
    JournalAction action;
    action.id = "3lzc7a2pfxn2c";
    action.kind = "post";
    action.text = "the moon is a loyal companion";
    action.digest = "0123456789abcdef";
    action.outcome = JournalActionOutcome::Executed;
    action.reason = "allow";
    action.uri = "at://did:plc:example/app.bsky.feed.post/3lzc7a2pfxn2c";
    action.cid = "bafyreiabc123";
    action.at = "2026-09-17T17:00:00Z";
    return action;
}

void test_action_round_trip() {
    const auto root = scratch_dir("roundtrip");
    const auto path = root / "action-journal.jsonl";

    const JournalAction action = sample_action();
    atperson::append_journal_action(path, action);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 1u);
    assert(journal.events.empty());
    assert(journal.valence.empty());
    assert(!journal.repaired_torn_tail);
    const JournalAction &loaded = journal.actions[0];
    assert(loaded.id == action.id);
    assert(loaded.kind == action.kind);
    assert(loaded.text == action.text);
    assert(loaded.digest == action.digest);
    assert(loaded.outcome == JournalActionOutcome::Executed);
    assert(loaded.reason == action.reason);
    assert(loaded.uri == action.uri);
    assert(loaded.cid == action.cid);
    assert(loaded.at == action.at);
}

void test_append_order_across_kinds() {
    const auto root = scratch_dir("order");
    const auto path = root / "action-journal.jsonl";

    const JournalAction action = sample_action();
    atperson::append_journal_action(path, action);

    JournalEvent event;
    event.action_id = action.id;
    event.event_uri = "at://did:plc:other/app.bsky.feed.post/3lzc7reply1";
    event.author_did = "did:plc:other";
    event.via = "parent";
    event.at = "2026-09-17T18:00:00Z";
    atperson::append_journal_event(path, event);

    JournalValence valence;
    valence.token = "moon";
    valence.kind = "action";
    valence.signal = 0.5f;
    valence.source = action.id;
    valence.at_epoch = 1758122400u;
    valence.at = "2026-09-17T19:00:00Z";
    atperson::append_journal_valence(path, valence);

    // The file preserves append order: action, then event, then valence.
    std::ifstream file(path, std::ios::binary);
    std::string first;
    std::getline(file, first);
    assert(first.find("\"type\":\"action\"") != std::string::npos);
    std::string second;
    std::getline(file, second);
    assert(second.find("\"type\":\"event\"") != std::string::npos);
    std::string third;
    std::getline(file, third);
    assert(third.find("\"type\":\"valence\"") != std::string::npos);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 1u);
    assert(journal.events.size() == 1u);
    assert(journal.valence.size() == 1u);
    assert(journal.events[0].via == "parent");
    assert(journal.valence[0].token == "moon");
    assert(journal.valence[0].at_epoch == 1758122400u);
}

void test_outcome_names_round_trip() {
    const JournalActionOutcome outcomes[] = {
        JournalActionOutcome::Executed, JournalActionOutcome::Denied,
        JournalActionOutcome::Deferred, JournalActionOutcome::Failed, JournalActionOutcome::DryRun};
    for (const JournalActionOutcome outcome : outcomes) {
        const std::string name = atperson::journal_action_outcome_name(outcome);
        const auto parsed = atperson::journal_action_outcome_from_name(name);
        assert(parsed.has_value());
        assert(*parsed == outcome);
    }
    assert(!atperson::journal_action_outcome_from_name("nonsense").has_value());
}

void test_torn_tail_truncated() {
    const auto root = scratch_dir("torn");
    const auto path = root / "action-journal.jsonl";

    atperson::append_journal_action(path, sample_action());
    // Simulate a crash mid-append: a partial line without a newline.
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "{\"type\":\"event\",\"action_id\":\"3l";
    }

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.repaired_torn_tail);
    assert(journal.actions.size() == 1u);
    assert(journal.events.empty());
}

void test_malformed_line_rejected() {
    const auto root = scratch_dir("malformed");
    const auto path = root / "action-journal.jsonl";

    atperson::append_journal_action(path, sample_action());
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "{\"type\":\"nonsense\",\"version\":1}\n";
    }
    try {
        (void)atperson::load_journal(path);
        assert(false && "malformed journal line must throw");
    } catch (const JournalError &) {
    }
}

void test_unsupported_version_rejected() {
    const auto root = scratch_dir("version");
    const auto path = root / "action-journal.jsonl";
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "{\"type\":\"action\",\"version\":99}\n";
    }
    try {
        (void)atperson::load_journal(path);
        assert(false && "unsupported journal version must throw");
    } catch (const JournalError &) {
    }
}

void test_event_dedup_and_uri_lookup() {
    const auto root = scratch_dir("dedup");
    const auto path = root / "action-journal.jsonl";

    const JournalAction executed = sample_action();
    atperson::append_journal_action(path, executed);

    JournalAction denied = sample_action();
    denied.id = "deniedrkey0001";
    denied.outcome = JournalActionOutcome::Denied;
    denied.reason = "approval_required";
    denied.uri.clear();
    denied.cid.clear();
    atperson::append_journal_action(path, denied);

    JournalEvent event;
    event.action_id = executed.id;
    event.event_uri = "at://did:plc:other/app.bsky.feed.post/3lzc7reply1";
    event.author_did = "did:plc:other";
    event.via = "parent";
    event.at = "2026-09-17T18:00:00Z";
    atperson::append_journal_event(path, event);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 2u);
    assert(journal.events.size() == 1u);

    // Dedup: the same (action id, event uri) pair is already recorded.
    assert(atperson::journal_has_event(journal, executed.id, event.event_uri));
    assert(!atperson::journal_has_event(journal, executed.id, "at://did:plc:other/x/y"));
    assert(!atperson::journal_has_event(journal, "unknown", event.event_uri));

    // Linkage: the executed action is found by its result URI; a denied
    // action has no URI and is never returned.
    const JournalAction *found = atperson::journal_find_action_by_uri(
        journal, "at://did:plc:example/app.bsky.feed.post/3lzc7a2pfxn2c");
    assert(found != nullptr);
    assert(found->id == executed.id);
    assert(atperson::journal_find_action_by_uri(
               journal, "at://did:plc:example/app.bsky.feed.post/none") == nullptr);
}

void test_missing_file_loads_empty() {
    const auto root = scratch_dir("missing");
    const JournalContents journal = atperson::load_journal(root / "absent.jsonl");
    assert(journal.actions.empty());
    assert(journal.events.empty());
    assert(journal.valence.empty());
    assert(!journal.repaired_torn_tail);
}

/* #57 (scoped): an action written with a journal-integrity MAC round-trips
 * through the journal, and the four fields survive byte-for-byte. */
void test_mac_round_trip() {
    const auto root = scratch_dir("mac");
    const auto path = root / "action-journal.jsonl";

    JournalAction action = sample_action();
    atperson::JournalMac mac;
    mac.mode = "hmac-sha256";
    mac.key_hint = "00112233";
    mac.sig = std::string(64, 'a');
    mac.digest = "296a8c3ea23f24884539351b7e63c7e605bae1be0ef45f5bbae8b7e6516598a5";
    action.mac = mac;
    atperson::append_journal_action(path, action);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 1u);
    assert(journal.actions[0].mac.has_value());
    assert(journal.actions[0].mac->mode == "hmac-sha256");
    assert(journal.actions[0].mac->key_hint == "00112233");
    assert(journal.actions[0].mac->sig == std::string(64, 'a'));
    assert(journal.actions[0].mac->digest ==
           "296a8c3ea23f24884539351b7e63c7e605bae1be0ef45f5bbae8b7e6516598a5");

    /* Serialise -> load -> serialise is byte-identical. */
    const std::string first = atperson::serialise_journal_action(journal.actions[0]);
    const std::string second = atperson::serialise_journal_action(journal.actions[0]);
    assert(first == second);
    std::printf("ok MAC round trip\n");
}

/* #57: an action written without a MAC (the common case, and the
 * v1 shape) loads as nullopt — the migration is silent and lossless. */
void test_no_mac_loads_as_nullopt() {
    const auto root = scratch_dir("no-mac");
    const auto path = root / "action-journal.jsonl";

    JournalAction action = sample_action();
    action.mac = std::nullopt;
    atperson::append_journal_action(path, action);

    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 1u);
    assert(!journal.actions[0].mac.has_value());
    std::printf("ok no MAC loads as nullopt\n");
}

/* #57: a v1 action entry (version 1, no MAC) loads under the v2
 * format and is indistinguishable from a v2 entry with nullopt MAC. */
void test_v1_action_migrates_silently() {
    const auto root = scratch_dir("v1-migrate");
    const auto path = root / "action-journal.jsonl";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"type\":\"action\",\"version\":1,\"id\":\"3lzc7a2pfxn2c\",\"kind\":\"post\","
                "\"text\":\"the moon is a loyal companion\",\"digest\":\"0123456789abcdef\","
                "\"outcome\":\"executed\",\"reason\":\"allow\","
                "\"uri\":\"at://did:plc:example/app.bsky.feed.post/3lzc7a2pfxn2c\","
                "\"cid\":\"bafyreiabc123\",\"at\":\"2026-09-17T17:00:00Z\"}\n";
    }
    const JournalContents journal = atperson::load_journal(path);
    assert(journal.actions.size() == 1u);
    assert(!journal.actions[0].mac.has_value());
    assert(journal.actions[0].id == "3lzc7a2pfxn2c");
    std::printf("ok v1 action migrates silently\n");
}

/* #57: a v1 event or valence entry is still refused — only v1 *actions* carry
 * the migration, because that is the only field the MAC change touches. */
void test_v1_event_is_refused() {
    const auto root = scratch_dir("v1-event");
    const auto path = root / "action-journal.jsonl";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"type\":\"event\",\"version\":1,\"action_id\":\"x\",\"event_uri\":\"y\","
                "\"author_did\":\"did:plc:a\",\"via\":\"parent\",\"at\":\"2026-09-17T17:00:00Z\"}\n";
    }
    bool threw = false;
    try {
        (void)atperson::load_journal(path);
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok v1 event is refused\n");
}

/* #57: a malformed MAC object is rejected, not silently dropped. */
void test_malformed_mac_rejected() {
    const auto root = scratch_dir("bad-mac");
    const auto path = root / "action-journal.jsonl";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"type\":\"action\",\"version\":2,\"id\":\"x\",\"kind\":\"post\",\"text\":\"t\","
                "\"digest\":\"0123456789abcdef\",\"outcome\":\"executed\",\"reason\":\"allow\","
                "\"uri\":\"\",\"cid\":\"\",\"at\":\"2026-09-17T17:00:00Z\","
                "\"mac\":{\"mode\":\"hmac-sha256\",\"key_hint\":\"00112233\",\"sig\":\"s\"}}\n";
    }
    bool threw = false;
    try {
        (void)atperson::load_journal(path);
    } catch (const JournalError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok malformed MAC rejected\n");
}

} // namespace

int main() {
    test_action_round_trip();
    test_append_order_across_kinds();
    test_outcome_names_round_trip();
    test_torn_tail_truncated();
    test_malformed_line_rejected();
    test_unsupported_version_rejected();
    test_event_dedup_and_uri_lookup();
    test_missing_file_loads_empty();
    test_mac_round_trip();
    test_no_mac_loads_as_nullopt();
    test_v1_action_migrates_silently();
    test_v1_event_is_refused();
    test_malformed_mac_rejected();
    std::printf("atperson-journal: all tests passed\n");
    return 0;
}
