#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-cpp-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

} // namespace

int main() {
    atperson::LanguageGraph graph;
    graph.observe("hello world hello", "at://example/post");

    const auto stats = graph.stats();
    assert(stats.node_count == 2u);
    assert(stats.edge_count == 2u);

    const auto associations = graph.associations("hello", 4u);
    assert(associations.size() == 1u);
    assert(associations.front().token == "world");

    atperson::LanguageGraph action_graph;
    action_graph.observe("alpha beta", "at://example/1");
    action_graph.observe("alpha beta", "at://example/2");
    action_graph.observe("delta beta", "at://example/3");
    action_graph.observe("alpha gamma", "at://example/4");
    const auto action_stats = action_graph.stats();
    const auto candidates = action_graph.action_candidates("alpha delta", 4u);
    assert(candidates.size() == 2u);
    assert(candidates.front().token == "beta");
    assert(candidates.front().context_matches == 2u);
    assert(candidates.front().supporting_observations == 3u);
    assert(candidates.front().score > 0.0f);
    assert(candidates.front().association_score > 0.0f);
    assert(candidates.front().familiarity_score > 0.0f);
    assert(candidates.front().support_score > 0.0f);
    assert(action_graph.stats().node_count == action_stats.node_count);
    assert(action_graph.stats().training_steps == action_stats.training_steps);
    assert(action_graph.action_candidates("unknown context", 4u).empty());
    assert(action_graph.action_candidates("alpha", 0u).empty());

    const std::filesystem::path path = "atperson-cpp-test.bin";
    graph.save(path);
    auto restored = atperson::LanguageGraph::load(path);
    assert(restored.stats().node_count == stats.node_count);
    std::filesystem::remove(path);

    const auto dir = scratch_dir("ledger");
    const auto ledger_path = dir / "ledger.bin";

    atperson::Ledger ledger(ledger_path);
    assert(ledger.count() == 0u);

    const std::string text = "durable dedup across runs";
    const auto digest = atperson::Ledger::digest(text);
    std::uint64_t id = 0u;
    assert(ledger.append("at://cpp/1", "did:plc:cpp", 100u, digest, ATPERSON_SCHEMA_VERSION,
                         ATP_LEDGER_OUTCOME_PENDING, &id) == atperson::LedgerResult::New);
    assert(id == 1u);
    ledger.set_outcome(id, ATP_LEDGER_OUTCOME_LEARNED);

    assert(ledger.append("at://cpp/1", "did:plc:cpp", 100u, digest, ATPERSON_SCHEMA_VERSION,
                         ATP_LEDGER_OUTCOME_PENDING,
                         &id) == atperson::LedgerResult::ExistsCommitted);

    atp_ledger_entry entry = {};
    assert(ledger.lookup("at://cpp/1", digest, &entry) == atperson::LedgerResult::ExistsCommitted);
    assert(entry.outcome == ATP_LEDGER_OUTCOME_LEARNED);
    assert(entry.content_digest == digest);
    assert(std::strcmp(entry.source_id, "at://cpp/1") == 0);

    atp_ledger_entry mirror = entry;
    graph.record_ledger_entry(mirror);
    const std::filesystem::path mirror_path = "atperson-cpp-mirror.bin";
    graph.save(mirror_path);
    auto mirrored = atperson::LanguageGraph::load(mirror_path);
    assert(mirrored.ledger_entries().size() == 1u);
    assert(mirrored.ledger_entries().front().content_digest == digest);
    std::filesystem::remove(mirror_path);

    std::filesystem::remove_all(dir);
    return 0;
}
