/* Network-native state (#142): record round-trips, the bounded publisher
 * drain, and the reconstruct acceptance path — publish then rebuild in a
 * fresh directory and verify the rebuilt ledger matches. All offline:
 * the writer and record source are fakes. */
#include "atperson/ledger.hpp"
#include "journal/store.hpp"
#include "replicate/publish.hpp"
#include "replicate/reconstruct.hpp"
#include "replicate/records.hpp"
#include "replicate/file_writer.hpp"
#include "support/fake_pds.hpp"
#include "thought/store.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-replicate-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

void test_record_round_trips() {
    atperson::ObservationRecord observation;
    observation.id = 42u;
    observation.source_id = "at://did:plc:author/app.bsky.feed.post/abc123";
    observation.author_did = "did:plc:author";
    observation.observed_at = 1234567890u;
    observation.content_digest = 0xDEADBEEFu;
    observation.schema_version = 2u;
    observation.outcome = "learned";
    observation.context.reply_parent_uri = "at://did:plc:other/app.bsky.feed.post/parent";
    const std::string json = atperson::serialise_observation_record(observation);
    const atperson::ObservationRecord parsed = atperson::parse_observation_record(json);
    assert(parsed.id == observation.id);
    assert(parsed.source_id == observation.source_id);
    assert(parsed.author_did == observation.author_did);
    assert(parsed.observed_at == observation.observed_at);
    assert(parsed.content_digest == observation.content_digest);
    assert(parsed.schema_version == observation.schema_version);
    assert(parsed.outcome == observation.outcome);
    assert(parsed.context.reply_parent_uri == observation.context.reply_parent_uri);
    assert(parsed.context.reply_root_uri.empty());
    assert(parsed.context.quote_uri.empty());

    atperson::ActionRecord action;
    action.id = "3labcdefg";
    action.kind = "post";
    action.text = "hello network";
    action.digest = "0123abcd";
    action.outcome = "executed";
    action.reason = "policy-allowed";
    action.uri = "at://did:fake/app.bsky.feed.post/xyz";
    action.cid = "bafyfake";
    action.at = "2026-09-24T00:00:00Z";
    const atperson::ActionRecord action_parsed =
        atperson::parse_action_record(atperson::serialise_action_record(action));
    assert(action_parsed.id == action.id);
    assert(action_parsed.text == action.text);
    assert(action_parsed.outcome == action.outcome);

    atperson::ValenceRecord valence;
    valence.token = "curious";
    valence.kind = "approach";
    valence.signal = 0.5f;
    valence.source = "operator";
    valence.at_epoch = 100u;
    valence.at = "2026-09-24T00:00:00Z";
    valence.provenance = "";
    const atperson::ValenceRecord valence_parsed =
        atperson::parse_valence_record(atperson::serialise_valence_record(valence));
    assert(valence_parsed.token == valence.token);
    assert(valence_parsed.signal == valence.signal);

    atperson::ThoughtRecord thought;
    thought.id = "aaaaaaaaaaaaa";
    thought.kind = "reflection";
    thought.text = "a note to my future self";
    thought.about_uri = "at://did:plc:a/app.bsky.feed.post/1";
    thought.at = "2026-09-24T00:00:00Z";
    const atperson::ThoughtRecord thought_parsed =
        atperson::parse_thought_record(atperson::serialise_thought_record(thought));
    assert(thought_parsed.id == thought.id);
    assert(thought_parsed.text == thought.text);
    assert(thought_parsed.about_uri == thought.about_uri);

    atperson::IntentRecord intent;
    intent.id = "3lintent1";
    intent.actions = {"3laction1"};
    intent.responder = "anyone";
    intent.expires_at_epoch = 1727136000u;
    intent.expires_at = "2026-09-24T00:00:00Z";
    intent.max_continuations = 3u;
    intent.state = "open";
    intent.at_epoch = 1727050000u;
    intent.at = "2026-09-23T00:06:40Z";
    const atperson::IntentRecord intent_parsed =
        atperson::parse_intent_record(atperson::serialise_intent_record(intent));
    assert(intent_parsed.id == intent.id);
    assert(intent_parsed.actions == intent.actions);
    assert(intent_parsed.responder == intent.responder);
    assert(intent_parsed.expires_at_epoch == intent.expires_at_epoch);
    assert(intent_parsed.expires_at == intent.expires_at);
    assert(intent_parsed.max_continuations == intent.max_continuations);
    assert(intent_parsed.state == intent.state);
    assert(intent_parsed.at_epoch == intent.at_epoch);
    assert(intent_parsed.at == intent.at);

    /* An unknown intent state is corruption, not a new state. */
    bool intent_state_threw = false;
    try {
        atperson::IntentRecord bad;
        bad.id = "3lintent2";
        bad.responder = "anyone";
        bad.state = "dormant";
        bad.at = "2026-09-24T00:00:00Z";
        atperson::serialise_intent_record(bad);
    } catch (const atperson::RecordError &) {
        intent_state_threw = true;
    }
    assert(intent_state_threw);

    /* An unknown thought kind is corruption, not a new vocabulary. */
    bool kind_threw = false;
    try {
        atperson::ThoughtRecord bad;
        bad.id = "aaaaaaaaaaaab";
        bad.kind = "musing";
        bad.text = "x";
        bad.at = "2026-09-24T00:00:00Z";
        atperson::serialise_thought_record(bad);
    } catch (const atperson::RecordError &) {
        kind_threw = true;
    }
    assert(kind_threw);

    /* Corruption is reported, not silently reinterpreted. */
    bool threw = false;
    try {
        atperson::parse_observation_record("{\"format\":\"nope\"}");
    } catch (const atperson::RecordError &) {
        threw = true;
    }
    assert(threw);
    std::puts("record round-trips: ok");
}

void test_drain_and_reconstruct() {
    const std::filesystem::path root = scratch_dir("roundtrip");
    const std::filesystem::path ledger_file = root / "ledger.bin";
    const std::filesystem::path journal_file = root / "action-journal.jsonl";
    const std::filesystem::path cursor_file = root / "cursor.json";

    /* Train: three observations, one withdrawn; one action; one valence. */
    atperson::Ledger ledger(ledger_file);
    std::uint64_t id1 = 0u;
    ledger.append("at://did:plc:a/app.bsky.feed.post/1", "did:plc:a", 100u,
                  atperson::Ledger::digest("first observation text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED,
                  "first observation text", {}, &id1);
    std::uint64_t id2 = 0u;
    ledger.append("at://did:plc:b/app.bsky.feed.post/2", "did:plc:b", 200u,
                  atperson::Ledger::digest("second observation text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED,
                  "second observation text", {}, &id2);
    std::uint64_t id3 = 0u;
    ledger.append("at://did:plc:a/app.bsky.feed.post/3", "did:plc:a", 300u,
                  atperson::Ledger::digest("third observation text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED,
                  "third observation text", {}, &id3);
    ledger.withdraw(id2);

    atperson::JournalAction action;
    action.id = "3laction1";
    action.kind = "post";
    action.text = "an outbound post";
    action.digest = "digest1";
    action.outcome = atperson::JournalActionOutcome::Executed;
    action.at = "2026-09-24T00:00:00Z";
    atperson::append_journal_action(journal_file, action);

    atperson::JournalValence valence;
    valence.token = "curious";
    valence.kind = "approach";
    valence.signal = 0.5f;
    valence.source = "operator";
    valence.at_epoch = 100u;
    valence.at = "2026-09-24T00:00:00Z";
    atperson::append_journal_valence(journal_file, valence);

    /* A thought: self-authored, published in full. */
    const std::filesystem::path thoughts_file = root / "thoughts";
    atperson::Thought thought;
    thought.id = "aaaaaaaaaaaaa";
    thought.kind = "reflection";
    thought.text = "the moon post made me curious";
    thought.at = "2026-09-24T00:00:00Z";
    atperson::write_thought(thoughts_file, thought);

    /* An intent: pending conversation state, mirrored to the network. */
    atperson::JournalIntent journal_intent;
    journal_intent.id = "3lintent1";
    journal_intent.actions = {"3laction1"};
    journal_intent.responder = "anyone";
    journal_intent.expires_at_epoch = 1727136000u;
    journal_intent.expires_at = "2026-09-24T00:00:00Z";
    journal_intent.max_continuations = 3u;
    journal_intent.state = atperson::IntentState::Open;
    journal_intent.at_epoch = 1727050000u;
    journal_intent.at = "2026-09-23T00:06:40Z";
    atperson::append_journal_intent(journal_file, journal_intent);

    /* Drain: everything published, withdrawal propagated. */
    atperson::e2e::FakePds writer;
    atperson::ReplicateConfig config;
    const atperson::ReplicateReport report =
        atperson::replicate_drain(cursor_file, journal_file, ledger, thoughts_file, writer,
                                  config);
    assert(report.network_failed == false);
    assert(report.observations_published == 3u);
    assert(report.actions_published == 1u);
    assert(report.valence_published == 1u);
    assert(report.thoughts_published == 1u);
    assert(report.intents_published == 1u);
    assert(report.withdrawal_updates == 0u);

    /* Second drain: the withdrawal of id2 propagates as an update. */
    const atperson::ReplicateReport second =
        atperson::replicate_drain(cursor_file, journal_file, ledger, thoughts_file, writer,
                                  config);
    assert(second.withdrawal_updates == 1u);
    assert(second.observations_published == 0u);

    /* Reconstruct into a fresh directory, reading the same store the
     * publisher wrote to. */
    atperson::e2e::FakePds &source = writer;
    source.serve_content("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    source.serve_content("at://did:plc:b/app.bsky.feed.post/2", "second observation text");
    source.serve_content("at://did:plc:a/app.bsky.feed.post/3", "third observation text");
    const std::filesystem::path fresh = root / "fresh";
    std::filesystem::create_directories(fresh);
    const atperson::ReconstructReport rebuilt =
        atperson::reconstruct_state(source, fresh / "ledger.bin", fresh / "journal.jsonl",
                                    fresh / "thoughts");
    assert(rebuilt.observations_replayed + rebuilt.observations_skipped_withdrawn == 3u);
    assert(rebuilt.observations_failed == 0u);
    assert(rebuilt.actions_replayed == 1u);
    assert(rebuilt.valence_replayed == 1u);
    assert(rebuilt.thoughts_replayed == 1u);
    assert(rebuilt.intents_replayed == 1u);
    assert(rebuilt.failures.empty());

    /* The rebuilt ledger matches: same entries, same outcomes, same
     * payloads, withdrawal excluded. */
    atperson::Ledger rebuilt_ledger(fresh / "ledger.bin");
    const std::vector<atp_ledger_entry> entries = rebuilt_ledger.entries();
    assert(entries.size() == 2u);
    assert(entries[0].outcome == ATP_LEDGER_OUTCOME_LEARNED);
    assert(entries[1].outcome == ATP_LEDGER_OUTCOME_LEARNED);
    assert(rebuilt_ledger.payload(entries[0].id) == "first observation text");
    assert(rebuilt_ledger.payload(entries[1].id) == "third observation text");

    const atperson::JournalContents journal = atperson::load_journal(fresh / "journal.jsonl");
    assert(journal.actions.size() == 1u);
    assert(journal.actions[0].text == "an outbound post");
    assert(journal.valence.size() == 1u);
    assert(journal.valence[0].token == "curious");
    assert(journal.intents.size() == 1u);
    assert(journal.intents[0].id == "3lintent1");
    assert(journal.intents[0].actions == std::vector<std::string>{"3laction1"});
    assert(journal.intents[0].responder == "anyone");
    assert(journal.intents[0].state == atperson::IntentState::Open);
    assert(journal.intents[0].max_continuations == 3u);

    const atperson::ThoughtContents rebuilt_thoughts =
        atperson::load_thoughts(fresh / "thoughts");
    assert(rebuilt_thoughts.thoughts.size() == 1u);
    assert(rebuilt_thoughts.thoughts[0].text == "the moon post made me curious");

    std::puts("drain + reconstruct round-trip: ok");
}

void test_drain_network_failure_retains_backlog() {
    const std::filesystem::path root = scratch_dir("failure");
    const std::filesystem::path ledger_file = root / "ledger.bin";
    const std::filesystem::path journal_file = root / "journal.jsonl";
    const std::filesystem::path cursor_file = root / "cursor.json";

    atperson::Ledger ledger(ledger_file);
    std::uint64_t id = 0u;
    ledger.append("at://did:plc:a/app.bsky.feed.post/1", "did:plc:a", 100u,
                  atperson::Ledger::digest("text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED, "text", {}, &id);

    atperson::e2e::FakePds writer;
    writer.fail_next_put();
    atperson::ReplicateConfig config;
    const atperson::ReplicateReport failed =
        atperson::replicate_drain(cursor_file, journal_file, ledger, root / "thoughts",
                                  writer, config);
    assert(failed.network_failed);
    assert(failed.observations_published == 0u);

    /* The cursor stayed at the start, so a later drain republishes. The
     * fixture clears its own failure when it throws, so nothing to reset. */
    const atperson::ReplicateReport retried =
        atperson::replicate_drain(cursor_file, journal_file, ledger, root / "thoughts",
                                  writer, config);
    assert(retried.network_failed == false);
    assert(retried.observations_published == 1u);
    std::puts("drain network failure retains backlog: ok");
}

void test_reconstruct_fail_closed() {
    const std::filesystem::path root = scratch_dir("failclosed");
    atperson::Ledger ledger(root / "ledger.bin");
    std::uint64_t id = 0u;
    ledger.append("at://did:plc:a/app.bsky.feed.post/1", "did:plc:a", 100u,
                  atperson::Ledger::digest("original text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED, "original text", {}, &id);

    atperson::e2e::FakePds writer;
    atperson::ReplicateConfig config;
    atperson::replicate_drain(root / "cursor.json", root / "journal.jsonl", ledger,
                              root / "thoughts", writer, config);

    /* The source now serves TAMPERED content for that URI: a digest
     * mismatch must fail closed. The records are untouched, so this is
     * tampering with the source, not with the network copy. */
    atperson::e2e::FakePds &source = writer;
    source.serve_content("at://did:plc:a/app.bsky.feed.post/1", "tampered text");
    const std::filesystem::path fresh = root / "fresh";
    std::filesystem::create_directories(fresh);
    const atperson::ReconstructReport rebuilt =
        atperson::reconstruct_state(source, fresh / "ledger.bin", fresh / "journal.jsonl",
                                    fresh / "thoughts");
    assert(rebuilt.observations_replayed == 0u);
    assert(rebuilt.observations_failed == 1u);
    assert(rebuilt.failures.size() == 1u);
    std::puts("reconstruct digest mismatch fails closed: ok");
}

void test_offline_drain_stages_record_files() {
    const std::filesystem::path root = scratch_dir("offline");
    const std::filesystem::path ledger_file = root / "ledger.bin";
    const std::filesystem::path journal_file = root / "journal.jsonl";
    const std::filesystem::path records_dir = root / "records";

    atperson::Ledger ledger(ledger_file);
    std::uint64_t id = 0u;
    ledger.append("at://did:plc:a/app.bsky.feed.post/1", "did:plc:a", 100u,
                  atperson::Ledger::digest("offline text"), 2u,
                  atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED, "offline text", {}, &id);

    atperson::Thought thought;
    thought.id = "bbbbbbbbbbbbb";
    thought.kind = "reflection";
    thought.text = "staged offline";
    thought.at = "2026-09-24T00:00:00Z";
    atperson::write_thought(root / "thoughts", thought);

    /* Offline drain: the exact record JSON lands under records/<nsid>/. */
    atperson::FileWriter writer(records_dir);
    atperson::ReplicateConfig config;
    const atperson::ReplicateReport report =
        atperson::replicate_drain(root / "cursor.json", journal_file, ledger,
                                  root / "thoughts", writer, config);
    assert(report.network_failed == false);
    assert(report.observations_published == 1u);
    assert(report.thoughts_published == 1u);

    const std::filesystem::path observation_file =
        records_dir / "click.croft.atperson.observation" / "aaaaaaaaaaaab.json";
    assert(std::filesystem::exists(observation_file));
    const std::filesystem::path thought_file =
        records_dir / "click.croft.atperson.thought" / "bbbbbbbbbbbbb.json";
    assert(std::filesystem::exists(thought_file));

    /* The staged bytes parse as valid records. */
    std::ifstream staged(thought_file);
    const std::string json{std::istreambuf_iterator<char>(staged),
                           std::istreambuf_iterator<char>()};
    const atperson::ThoughtRecord staged_record = atperson::parse_thought_record(json);
    assert(staged_record.text == "staged offline");

    /* A second offline drain is a no-op: the cursor advanced. */
    const atperson::ReplicateReport second =
        atperson::replicate_drain(root / "cursor.json", journal_file, ledger,
                                  root / "thoughts", writer, config);
    assert(second.observations_published == 0u);
    assert(second.thoughts_published == 0u);
    std::puts("offline drain stages record files: ok");
}

} // namespace

int main() {
    test_record_round_trips();
    test_drain_and_reconstruct();
    test_drain_network_failure_retains_backlog();
    test_reconstruct_fail_closed();
    test_offline_drain_stages_record_files();
    return 0;
}
