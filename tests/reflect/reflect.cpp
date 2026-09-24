/* Deterministic reflection (#151): the bounded, non-training thought-surface
 * module and its CLI atoms.
 *
 * Covers: thought store round-trip / kinds / optional provenance /
 * one-record-per-file layout / id/file agreement / malformed refusal; the
 * reflection pass valence trigger on a bare journal (issue acceptance, no
 * scheduled consolidation), trigger dedup, bounded burst (max_thoughts),
 * consolidation cadence gating, the unfamiliar-author and reply-ratio
 * triggers, and non-training read-only guarantees; plus the `thought` /
 * `thoughts` / `reflect` CLI entry points and their listing filters. Offline,
 * no network; the clock is injected. */
#include "cli/thoughts.hpp"

#include "atperson/graph.hpp"
#include "journal/store.hpp"
#include "reflect/config.hpp"
#include "reflect/pass.hpp"
#include "state/time.hpp"
#include "thought/store.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using atperson::JournalContents;
using atperson::JournalEvent;
using atperson::JournalValence;
using atperson::LanguageGraph;
using atperson::ReflectionConfig;
using atperson::ReflectionReport;
using atperson::Thought;
using atperson::ThoughtContents;

constexpr std::int64_t NOW = 1'700'000'000;
constexpr std::int64_t WINDOW = 604'800;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-reflect-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string now_string() { return atperson::rfc3339_from_unix(NOW); }

ReflectionConfig config(bool enable_consolidation = true) {
    ReflectionConfig config;
    config.enabled = true;
    config.cadence_seconds =
        enable_consolidation ? 100'000'000 : 100'000'000; /* huge: consolidation off */
    config.max_thoughts = 8u;
    config.window_seconds = WINDOW;
    config.valence_delta_min = 0.25f;
    config.unfamiliar_authors_min = 2u;
    config.reply_ratio_shift_min = 0.25f;
    return config;
}

/* A consolidation whose `at` is exactly `now`: the pass must not write
 * another one under the huge cadence, isolating trigger behaviour. */
void seed_consolidation(const std::filesystem::path &thoughts_path, std::int64_t now) {
    Thought seed;
    seed.id = atperson::new_thought_id();
    seed.kind = "consolidation";
    seed.text = "seeded";
    seed.at = atperson::rfc3339_from_unix(now);
    seed.span_start = seed.at;
    seed.span_end = seed.at;
    atperson::write_thought(thoughts_path, seed);
}

std::string journal_bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void append_valence(const std::filesystem::path &path, std::string_view token,
                    std::string_view kind, float signal, std::int64_t at) {
    JournalValence entry;
    entry.token = std::string(token);
    entry.kind = std::string(kind);
    entry.signal = signal;
    entry.at = atperson::rfc3339_from_unix(at);
    entry.at_epoch = static_cast<std::uint64_t>(at);
    atperson::append_journal_valence(path, entry);
}

void append_event(const std::filesystem::path &path, std::string_view via, std::int64_t at) {
    JournalEvent entry;
    entry.action_id = "at://reflect/action";
    entry.event_uri = "at://reflect/event/" + std::to_string(at);
    entry.author_did = "did:plc:other";
    entry.via = std::string(via);
    entry.at = atperson::rfc3339_from_unix(at);
    atperson::append_journal_event(path, entry);
}

/* --- thought store ---------------------------------------------------- */

int test_store_round_trip() {
    const auto dir = scratch_dir("store");
    const auto store = dir / "thoughts";

    Thought reflection;
    reflection.id = "2nd3rd4rd5rd6rd7rd8rd9";
    reflection.kind = "reflection";
    reflection.text = "noted for later";
    reflection.at = now_string();
    atperson::write_thought(store, reflection);

    Thought consolidation;
    consolidation.id = "2nd3rd4rd5rd6rd7rd8rd9a";
    consolidation.kind = "consolidation";
    consolidation.text = "periodic reflection summary";
    consolidation.at = now_string();
    consolidation.span_start = atperson::rfc3339_from_unix(NOW - WINDOW);
    consolidation.span_end = now_string();
    consolidation.topic = "consolidation";
    atperson::write_thought(store, consolidation);

    /* One JSON record per file, named by record key. */
    if (!std::filesystem::exists(store / "2nd3rd4rd5rd6rd7rd8rd9.json") ||
        !std::filesystem::exists(store / "2nd3rd4rd5rd6rd7rd8rd9a.json")) {
        std::cerr << "FAIL store one-record-per-file layout\n";
        return 1;
    }

    const ThoughtContents contents = atperson::load_thoughts(store);
    if (contents.thoughts.size() != 2u) {
        std::cerr << "FAIL store round-trip count\n";
        return 1;
    }
    const Thought &loaded = contents.thoughts[0];
    if (loaded.id != reflection.id || loaded.kind != "reflection" || loaded.text != "noted for later" ||
        loaded.at != now_string() || !loaded.span_start.empty() || !loaded.topic.empty()) {
        std::cerr << "FAIL store reflection fields\n";
        return 1;
    }
    const Thought &provenance = contents.thoughts[1];
    if (provenance.kind != "consolidation" || provenance.span_end != now_string() ||
        provenance.span_start != atperson::rfc3339_from_unix(NOW - WINDOW) ||
        provenance.topic != "consolidation") {
        std::cerr << "FAIL store provenance fields\n";
        return 1;
    }

    /* Records are immutable: writing the same id again is refused. */
    bool duplicate_refused = false;
    try {
        atperson::write_thought(store, reflection);
    } catch (const std::runtime_error &) {
        duplicate_refused = true;
    }
    if (!duplicate_refused) {
        std::cerr << "FAIL store accepts a duplicate record id\n";
        return 1;
    }

    /* Invalid kinds are refused on both write and parse. */
    Thought bad = reflection;
    bad.id = "2nd3rd4rd5rd6rd7rd8rd9b";
    bad.kind = "invention";
    bool refused = false;
    try {
        atperson::write_thought(store, bad);
    } catch (const atperson::ThoughtError &) {
        refused = true;
    }
    if (!refused) {
        std::cerr << "FAIL store accepts invalid kind\n";
        return 1;
    }

    /* Non-record files (a leftover `<id>.json.tmp`, any other extension) are
     * ignored by the reader. */
    {
        std::ofstream leftover(store / "2nd3rd4rd5rd6rd7rd8rd9.json.tmp",
                               std::ios::binary | std::ios::app);
        leftover << "{\"format\":\"atperson-thought\",\"version\":1}";
    }
    {
        std::ofstream notes(store / "notes.txt", std::ios::binary | std::ios::app);
        notes << "not a record";
    }
    const ThoughtContents with_junk = atperson::load_thoughts(store);
    if (with_junk.thoughts.size() != 2u) {
        std::cerr << "FAIL store reads non-record files (loaded " << with_junk.thoughts.size()
                  << ")\n";
        return 1;
    }

    /* A malformed record is corruption: it must fail loudly. */
    {
        const auto bad_store = dir / "bad";
        std::filesystem::create_directories(bad_store);
        std::ofstream file(bad_store / "zzzzzzzzzzzzz.json", std::ios::binary | std::ios::trunc);
        file << "{\"format\":\"atperson-thought\",\"version\":1,\"kind\":\"reflection\","
                "\"text\":\"missing id and at\",\"about\":null}";
    }
    bool malformed = false;
    try {
        (void)atperson::load_thoughts(dir / "bad");
    } catch (const atperson::ThoughtError &) {
        malformed = true;
    }
    if (!malformed) {
        std::cerr << "FAIL store accepts a malformed record on load\n";
        return 1;
    }

    /* A record whose id disagrees with its filename is relocation, and fails
     * loudly rather than silently reindexing. */
    {
        const auto moved_store = dir / "moved";
        std::filesystem::create_directories(moved_store);
        std::ofstream file(moved_store / "different.json", std::ios::binary | std::ios::trunc);
        file << atperson::serialise_thought(reflection);
    }
    bool relocated = false;
    try {
        (void)atperson::load_thoughts(dir / "moved");
    } catch (const atperson::ThoughtError &) {
        relocated = true;
    }
    if (!relocated) {
        std::cerr << "FAIL store accepts a record that disagrees with its filename\n";
        return 1;
    }
    return 0;
}

/* --- reflection pass -------------------------------------------------- */

int test_valence_trigger_without_scheduled_pass() {
    const auto dir = scratch_dir("trigger");
    const auto thoughts_path = dir / "thoughts";
    const auto journal_path = dir / "action-journal.jsonl";
    seed_consolidation(thoughts_path, NOW);
    append_valence(journal_path, "moon", "interaction", 0.5f, NOW);

    LanguageGraph graph;
    const ReflectionReport report =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      config(), static_cast<std::uint64_t>(NOW));
    if (report.thoughts_written != 1u || report.written.size() != 1u) {
        std::cerr << "FAIL valence trigger wrote " << report.thoughts_written << " thought(s)\n";
        return 1;
    }
    const Thought &movement = report.written[0].thought;
    if (movement.kind != "movement" || movement.topic != "valence:interaction:moon" ||
        movement.span_end != now_string()) {
        std::cerr << "FAIL valence trigger fields\n";
        return 1;
    }
    if (movement.text.find("moon") == std::string::npos ||
        movement.text.find("interaction") == std::string::npos ||
        movement.text.find("+0.50") == std::string::npos ||
        movement.text.find("1 update(s)") == std::string::npos) {
        std::cerr << "FAIL valence trigger text '" << movement.text << "'\n";
        return 1;
    }
    if (report.valence_updates_in_window != 1u || report.valence_tokens_in_window != 1u) {
        std::cerr << "FAIL valence window stats\n";
        return 1;
    }

    /* Deterministic: the same clock writes nothing twice. */
    const ReflectionReport rerun =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      config(), static_cast<std::uint64_t>(NOW));
    if (rerun.thoughts_written != 0u) {
        std::cerr << "FAIL rerun wrote " << rerun.thoughts_written << " thought(s)\n";
        return 1;
    }
    const ThoughtContents contents = atperson::load_thoughts(thoughts_path);
    std::size_t movements = 0u;
    for (const Thought &entry : contents.thoughts) {
        if (entry.kind == "movement") {
            ++movements;
        }
    }
    if (movements != 1u) {
        std::cerr << "FAIL stored movement count " << movements << '\n';
        return 1;
    }
    return 0;
}

int test_bounded_burst() {
    const auto dir = scratch_dir("burst");
    const auto thoughts_path = dir / "thoughts";
    const auto journal_path = dir / "action-journal.jsonl";
    seed_consolidation(thoughts_path, NOW);
    for (int i = 0; i < 10; ++i) {
        append_valence(journal_path, "t" + std::to_string(i), "interaction", 0.5f, NOW);
    }
    ReflectionConfig bounded = config();
    bounded.max_thoughts = 3u;

    LanguageGraph graph;
    const ReflectionReport report =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      bounded, static_cast<std::uint64_t>(NOW));
    if (report.thoughts_written != 3u) {
        std::cerr << "FAIL bounded burst wrote " << report.thoughts_written << "\n";
        return 1;
    }
    const ThoughtContents contents = atperson::load_thoughts(thoughts_path);
    std::size_t movements = 0u;
    for (const Thought &entry : contents.thoughts) {
        if (entry.kind == "movement") {
            ++movements;
        }
    }
    if (movements != 3u) {
        std::cerr << "FAIL bounded movements stored " << movements << "\n";
        return 1;
    }
    const ReflectionReport rerun =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      bounded, static_cast<std::uint64_t>(NOW));
    if (rerun.thoughts_written != 0u) {
        std::cerr << "FAIL bounded rerun wrote " << rerun.thoughts_written << "\n";
        const ThoughtContents dump = atperson::load_thoughts(thoughts_path);
        for (const Thought &entry : dump.thoughts) {
            std::cerr << "  store: kind=" << entry.kind << " topic=[" << entry.topic
                      << "] span_end=[" << entry.span_end << "]\n";
        }
        return 1;
    }
    return 0;
}

int test_consolidation_cadence() {
    const auto dir = scratch_dir("cadence");
    const auto thoughts_path = dir / "thoughts";
    const auto journal_path = dir / "action-journal.jsonl";

    ReflectionConfig cadent = config();
    cadent.cadence_seconds = 100;
    LanguageGraph graph;

    const ReflectionReport first =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      cadent, static_cast<std::uint64_t>(NOW));
    if (first.thoughts_written != 1u || first.written[0].thought.kind != "consolidation") {
        std::cerr << "FAIL first consolidation\n";
        return 1;
    }
    /* An empty window still produces a real summary; stats are honest zero. */
    if (first.valence_updates_in_window != 0u || first.episodes_in_window != 0u ||
        first.authors_in_window != 0u || first.events_in_window != 0u) {
        std::cerr << "FAIL empty-window stats\n";
        return 1;
    }

    if (atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      cadent, static_cast<std::uint64_t>(NOW))
            .thoughts_written != 0u) {
        std::cerr << "FAIL consolidation wrote twice at same instant\n";
        return 1;
    }
    if (atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      cadent, static_cast<std::uint64_t>(NOW + 60))
            .thoughts_written != 0u) {
        std::cerr << "FAIL consolidation wrote before cadence elapsed\n";
        return 1;
    }
    const ReflectionReport after_cadence =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      cadent, static_cast<std::uint64_t>(NOW + 100));
    if (after_cadence.thoughts_written != 1u) {
        std::cerr << "FAIL consolidation not due at cadence boundary\n";
        return 1;
    }
    const ThoughtContents contents = atperson::load_thoughts(thoughts_path);
    std::size_t consolidations = 0u;
    for (const Thought &entry : contents.thoughts) {
        if (entry.kind == "consolidation") {
            ++consolidations;
        }
    }
    if (consolidations != 2u) {
        std::cerr << "FAIL stored consolidations " << consolidations << "\n";
        return 1;
    }
    return 0;
}

int test_unfamiliar_authors_trigger() {
    const auto dir = scratch_dir("authors");
    const auto thoughts_path = dir / "thoughts";
    seed_consolidation(thoughts_path, NOW);

    LanguageGraph graph;
    /* Two distinct authors, both with zero ledger exposure. */
    {
        bool a = graph.remember("alpha beta", "at://reflect/a", "did:plc:alice", NOW, 12345u,
                                1u, 900u);
        bool b = graph.remember("gamma delta", "at://reflect/b", "did:plc:bob", NOW, 67890u, 1u,
                                901u);
        if (!a || !b) {
            std::cerr << "FAIL remember did not create episodes\n";
            return 1;
        }
    }
    if (graph.episodes().size() != 2u || !graph.ledger_entries().empty()) {
        std::cerr << "FAIL author fixture shape\n";
        return 1;
    }

    const ReflectionReport report =
        atperson::run_reflection_pass(thoughts_path, atperson::JournalContents(), graph, config(),
                                      static_cast<std::uint64_t>(NOW));
    if (report.thoughts_written != 1u || report.written[0].thought.topic != "authors") {
        std::cerr << "FAIL authors trigger\n";
        return 1;
    }
    const std::string &text = report.written[0].thought.text;
    if (text.find("2 author(s)") == std::string::npos ||
        text.find("did:plc:alice") == std::string::npos ||
        text.find("did:plc:bob") == std::string::npos) {
        std::cerr << "FAIL authors trigger text '" << text << "'\n";
        return 1;
    }
    if (report.episodes_in_window != 2u || report.authors_in_window != 2u) {
        std::cerr << "FAIL authors window stats\n";
        return 1;
    }
    return 0;
}

int test_reply_ratio_trigger() {
    const auto dir = scratch_dir("reply");
    const auto thoughts_path = dir / "thoughts";
    const auto journal_path = dir / "action-journal.jsonl";
    seed_consolidation(thoughts_path, NOW);

    /* Previous window: 10 events, 1 reply (10% replies). */
    for (int i = 0; i < 10; ++i) {
        append_event(journal_path, i == 0 ? "reply" : "post", NOW - WINDOW - 50);
    }
    /* Trailing window: 5 events, 4 replies (80% replies). */
    for (int i = 0; i < 5; ++i) {
        append_event(journal_path, i < 4 ? "reply" : "post", NOW - 50);
    }

    LanguageGraph graph;
    const ReflectionReport report =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      config(), static_cast<std::uint64_t>(NOW));
    if (report.thoughts_written != 1u || report.written[0].thought.topic != "reply") {
        std::cerr << "FAIL reply trigger wrote " << report.thoughts_written << "\n";
        return 1;
    }
    const std::string &text = report.written[0].thought.text;
    if (text.find("reply ratio shifted") == std::string::npos ||
        text.find("80%") == std::string::npos || text.find("10%") == std::string::npos) {
        std::cerr << "FAIL reply trigger text '" << text << "'\n";
        return 1;
    }
    if (report.events_in_window != 5u) {
        std::cerr << "FAIL reply window stats\n";
        return 1;
    }
    return 0;
}

int test_non_training_read_only() {
    const auto dir = scratch_dir("readonly");
    const auto thoughts_path = dir / "thoughts";
    const auto journal_path = dir / "action-journal.jsonl";
    seed_consolidation(thoughts_path, NOW);
    append_valence(journal_path, "moon", "interaction", 0.5f, NOW);
    const std::string journal_before = journal_bytes(journal_path);

    LanguageGraph graph;
    graph.observe("alpha beta gamma", "at://reflect/observe");
    const std::size_t episodes_before = graph.episodes().size();

    const ReflectionReport report =
        atperson::run_reflection_pass(thoughts_path, atperson::load_journal(journal_path), graph,
                                      config(), static_cast<std::uint64_t>(NOW));
    if (report.thoughts_written != 1u || report.written[0].thought.kind != "movement") {
        std::cerr << "FAIL read-only fixture shape\n";
        return 1;
    }
    if (graph.episodes().size() != episodes_before) {
        std::cerr << "FAIL pass changed episodes\n";
        return 1;
    }
    if (journal_bytes(journal_path) != journal_before) {
        std::cerr << "FAIL pass changed the journal\n";
        return 1;
    }
    {
        const JournalContents journal = atperson::load_journal(journal_path);
        if (journal.valence.size() != 1u) {
            std::cerr << "FAIL pass changed valence\n";
            return 1;
        }
    }
    return 0;
}

/* --- CLI atoms -------------------------------------------------------- */

int test_cli_surface() {
    const auto dir = scratch_dir("cli");
    const auto journal_path = dir / "action-journal.jsonl";

    std::ostringstream record;
    if (atperson::cli::run_thought_record(record, dir, {"hello", "world"}, now_string()) != 0) {
        std::cerr << "FAIL thought record\n";
        return 1;
    }
    if (record.str().find("recorded ") == std::string::npos ||
        record.str().find("hello world") == std::string::npos) {
        std::cerr << "FAIL thought record output '" << record.str() << "'\n";
        return 1;
    }

    std::ostringstream list;
    const std::string_view list_args[] = {"5"};
    if (atperson::cli::run_thoughts_list(list, dir, list_args, 1u) != 0 ||
        list.str().find("hello world") == std::string::npos) {
        std::cerr << "FAIL thoughts list\n";
        return 1;
    }

    const std::string_view kind_args[] = {"--kind", "movement"};
    std::ostringstream kind_list;
    if (atperson::cli::run_thoughts_list(kind_list, dir, kind_args, 2u) != 0 ||
        !kind_list.str().empty()) {
        std::cerr << "FAIL thoughts --kind filter\n";
        return 1;
    }

    const std::string since = now_string();
    const std::string_view since_args[] = {"--since", since};
    std::ostringstream since_list;
    if (atperson::cli::run_thoughts_list(since_list, dir, since_args, 2u) != 0 ||
        since_list.str().find("hello world") == std::string::npos) {
        std::cerr << "FAIL thoughts --since filter\n";
        return 1;
    }
    const std::string future = atperson::rfc3339_from_unix(NOW + 100);
    const std::string_view future_args[] = {"--since", future};
    std::ostringstream future_list;
    if (atperson::cli::run_thoughts_list(future_list, dir, future_args, 2u) != 0 ||
        !future_list.str().empty()) {
        std::cerr << "FAIL thoughts --since future filter\n";
        return 1;
    }

    /* No seeded consolidation: an explicit reflect writes one. */
    LanguageGraph graph;
    std::ostringstream reflect;
    if (atperson::cli::run_reflect_command(reflect, dir, journal_path, graph, NOW, now_string()) !=
        0) {
        std::cerr << "FAIL reflect command\n";
        return 1;
    }
    if (reflect.str().find("wrote 1 thought(s)") == std::string::npos ||
        reflect.str().find("consolidation") == std::string::npos) {
        std::cerr << "FAIL reflect command output '" << reflect.str() << "'\n";
        return 1;
    }

    /* Bad numeric limit is refused. */
    const std::string_view bad_args[] = {"abc"};
    std::ostringstream bad_list;
    if (atperson::cli::run_thoughts_list(bad_list, dir, bad_args, 1u) == 0) {
        std::cerr << "FAIL thoughts accepts a bad limit\n";
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const int tests[] = {
        test_store_round_trip(),
        test_valence_trigger_without_scheduled_pass(),
        test_bounded_burst(),
        test_consolidation_cadence(),
        test_unfamiliar_authors_trigger(),
        test_reply_ratio_trigger(),
        test_non_training_read_only(),
        test_cli_surface(),
    };
    int failed = 0;
    for (const int result : tests) {
        failed += result != 0;
    }
    if (failed != 0) {
        std::cerr << failed << " reflection test(s) failed\n";
        return 1;
    }
    std::cout << "reflect: all tests passed\n";
    return 0;
}