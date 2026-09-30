/* Reconstruct rebuilds fresh state from records fetched over the network. It
 * already treats observation content as untrusted (digest-verified); this
 * hardens the rest: record keys and ids never become paths outside the target
 * directories, one bad record never aborts recovery, and hostile field values
 * are reported and skipped. Offline, with a scripted hostile RecordSource. */

#include "atperson/ledger.hpp"
#include "journal/store.hpp"
#include "replicate/reconstruct.hpp"
#include "replicate/records.hpp"
#include "thought/store.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace atperson;

class ScriptedSource final : public RecordSource {
  public:
    void add(std::string collection, std::string rkey, std::string json) {
        listing_[collection].push_back(rkey);
        records_[{std::move(collection), std::move(rkey)}] = std::move(json);
    }
    void serve(std::string uri, std::string text) { content_[std::move(uri)] = std::move(text); }

    std::vector<std::string> list_records(std::string_view collection) override {
        const auto found = listing_.find(std::string(collection));
        return found == listing_.end() ? std::vector<std::string>{} : found->second;
    }
    std::optional<std::string> get_record(std::string_view collection,
                                          std::string_view rkey) override {
        const auto found = records_.find({std::string(collection), std::string(rkey)});
        if (found == records_.end()) {
            return std::nullopt;
        }
        return found->second;
    }
    std::optional<std::string> fetch_content(std::string_view source_uri) override {
        const auto found = content_.find(std::string(source_uri));
        if (found == content_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

  private:
    std::map<std::string, std::vector<std::string>> listing_;
    std::map<std::pair<std::string, std::string>, std::string> records_;
    std::map<std::string, std::string> content_;
};

std::filesystem::path scratch(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("atperson-reconstruct-hardening-" + std::string(tag) + "-" +
                       std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::size_t files_under(const std::filesystem::path &root) {
    std::size_t count = 0u;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

std::string observation_json(std::uint64_t id, const std::string &source_id,
                             const std::string &author, const std::string &content,
                             ConversationContext context = {}) {
    ObservationRecord record;
    record.id = id;
    record.source_id = source_id;
    record.author_did = author;
    record.observed_at = 100u + id;
    record.content_digest = Ledger::digest(content);
    record.schema_version = 2u;
    record.outcome = "learned";
    record.context = std::move(context);
    return serialise_observation_record(record);
}

std::string thought_json(const std::string &id) {
    ThoughtRecord record;
    record.id = id;
    record.kind = "reflection";
    record.text = "a thought";
    record.at = "2026-09-24T00:00:00Z";
    return serialise_thought_record(record);
}

/* A hostile listing can name rkeys that are paths. The thought id doubles as a
 * file name, so "../x" must not create a file outside the thoughts directory
 * (and must not be honoured even though the record's id matches the rkey). */
void test_path_traversal_rkeys_cannot_escape() {
    const auto root = scratch("traversal");
    const auto sandbox = root / "state";
    std::filesystem::create_directories(sandbox);
    const auto thoughts = sandbox / "thoughts";

    ScriptedSource source;
    for (const char *evil : {"../escaped", "../../escaped", "a/b", "/abs/escaped", "..",
                             ".", "with space", "nul\\u0000x", "x\\ny"}) {
        source.add(std::string(kThoughtCollection), evil, thought_json(evil));
        source.add(std::string(kIntentCollection), evil, "{}");
        source.add(std::string(kActionCollection), evil, "{}");
        source.add(std::string(kValenceCollection), evil, "{}");
        source.add(std::string(kObservationCollection), evil, "{}");
    }
    source.add(std::string(kThoughtCollection), "aaaaaaaaaaaaa", thought_json("aaaaaaaaaaaaa"));

    const ReconstructReport report = reconstruct_state(source, sandbox / "ledger.bin",
                                                       sandbox / "journal.jsonl", thoughts);
    assert(report.thoughts_replayed == 1u); /* only the well-formed key */
    assert(report.records_corrupt >= 9u);

    /* Nothing was created outside the state directory or its thoughts store. */
    assert(!std::filesystem::exists(root / "escaped.json"));
    assert(!std::filesystem::exists(sandbox / "escaped.json"));
    assert(!std::filesystem::exists("/abs/escaped.json"));
    assert(std::filesystem::exists(thoughts / "aaaaaaaaaaaaa.json"));
    for (const auto &entry : std::filesystem::directory_iterator(thoughts)) {
        assert(entry.path().parent_path() == thoughts);
    }
    std::filesystem::remove_all(root);
}

/* The storage boundary itself refuses ids that are not plain file names, so the
 * guarantee does not depend on every caller remembering to validate. */
void test_record_store_refuses_non_plain_ids() {
    const auto root = scratch("store");
    const auto dir = root / "store";
    Thought thought;
    thought.kind = "reflection";
    thought.text = "t";
    thought.at = "2026-09-24T00:00:00Z";
    for (const char *id : {"../x", "a/b", "..", ".", "", "a\\\\b", "x y"}) {
        thought.id = id;
        bool refused = false;
        try {
            write_thought(dir, thought);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
    }
    assert(!std::filesystem::exists(root / "x.json"));
    thought.id = "aaaaaaaaaaaab";
    write_thought(dir, thought);
    assert(thought_record_exists(dir, "aaaaaaaaaaaab"));
    assert(!thought_record_exists(dir, "../store/aaaaaaaaaaaab")); /* not even queryable */
    std::filesystem::remove_all(root);
}

/* One over-long or malformed observation must not abort recovery of the rest. */
void test_bad_observations_are_skipped_not_fatal() {
    const auto root = scratch("observations");
    ScriptedSource source;
    const std::string good_uri = "at://did:plc:a/app.bsky.feed.post/1";
    source.add(std::string(kObservationCollection), "aa", observation_json(1u, good_uri, "did:plc:a", "kept text"));
    source.serve(good_uri, "kept text");

    const std::string long_uri = "at://did:plc:a/app.bsky.feed.post/" + std::string(400u, 'x');
    source.add(std::string(kObservationCollection), "ab", observation_json(2u, long_uri, "did:plc:a", "long"));
    source.serve(long_uri, "long");

    const std::string bad_author_uri = "at://did:plc:b/app.bsky.feed.post/2";
    source.add(std::string(kObservationCollection), "ac", observation_json(3u, bad_author_uri, "didplc:b", "author"));
    source.serve(bad_author_uri, "author");

    const std::string long_author_uri = "at://did:plc:c/app.bsky.feed.post/3";
    source.add(std::string(kObservationCollection), "ad",
               observation_json(4u, long_author_uri, "did:plc:" + std::string(300u, 'a'), "longauthor"));
    source.serve(long_author_uri, "longauthor");

    const std::string context_uri = "at://did:plc:d/app.bsky.feed.post/4";
    ConversationContext context;
    context.quote_uri = "at://did:plc:d/app.bsky.feed.post/" + std::string(400u, 'q');
    source.add(std::string(kObservationCollection), "ae", observation_json(5u, context_uri, "did:plc:d", "ctx", context));
    source.serve(context_uri, "ctx");

    const std::string huge_uri = "at://did:plc:e/app.bsky.feed.post/5";
    const std::string huge(ATPERSON_LEDGER_PAYLOAD_LIMIT + 1u, 'h');
    source.add(std::string(kObservationCollection), "af", observation_json(6u, huge_uri, "did:plc:e", huge));
    source.serve(huge_uri, huge);

    const std::string last_uri = "at://did:plc:f/app.bsky.feed.post/6";
    source.add(std::string(kObservationCollection), "ag", observation_json(7u, last_uri, "did:plc:f", "also kept"));
    source.serve(last_uri, "also kept");

    const ReconstructReport report = reconstruct_state(
        source, root / "ledger.bin", root / "journal.jsonl", root / "thoughts");
    /* The good ones before and after the hostile ones both replay. */
    assert(report.observations_replayed == 2u);
    assert(report.observations_failed == 5u);

    Ledger ledger(root / "ledger.bin");
    assert(ledger.count() == 2u);
    std::filesystem::remove_all(root);
}

/* Garbage of every shape in every collection: recovery must finish, count the
 * garbage as corrupt, and leave a consistent, reopenable state. */
void test_garbage_in_every_collection_is_reported() {
    const auto root = scratch("garbage");
    ScriptedSource source;
    const char *garbage[] = {"", "null", "[]", "{", "{\"format\":\"atperson-record\"}",
                             "\"string\"", "{\"format\":\"nope\",\"version\":1}"};
    int n = 0;
    for (const auto &collection : {kObservationCollection, kActionCollection, kValenceCollection,
                                   kThoughtCollection, kIntentCollection}) {
        for (const char *json : garbage) {
            const std::string rkey = "k" + std::to_string(n++);
            source.add(std::string(collection), rkey, json);
        }
    }
    const ReconstructReport report = reconstruct_state(
        source, root / "ledger.bin", root / "journal.jsonl", root / "thoughts");
    assert(report.observations_replayed == 0u && report.actions_replayed == 0u &&
           report.valence_replayed == 0u && report.thoughts_replayed == 0u &&
           report.intents_replayed == 0u);
    assert(report.records_corrupt == 5u * (sizeof(garbage) / sizeof(garbage[0])));
    Ledger ledger(root / "ledger.bin");
    assert(ledger.count() == 0u);
    assert(files_under(root) <= 3u); /* ledger, marker, nothing else */
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_record_store_refuses_non_plain_ids();
    test_path_traversal_rkeys_cannot_escape();
    test_bad_observations_are_skipped_not_fatal();
    test_garbage_in_every_collection_is_reported();
    std::puts("reconstruct hardening tests passed");
    return 0;
}
