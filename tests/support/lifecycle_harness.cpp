/* Shared fixtures for the deterministic end-to-end lifecycle harness.
 * See lifecycle_harness.hpp for the contract; this file owns the
 * implementations so the harness types can be used from several
 * scenario translation units without duplicating them. */

#include "support/lifecycle_harness.hpp"

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "control/state.hpp"
#include "ingestion/state.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace atperson::e2e {

std::filesystem::path scratch_dir(std::string_view tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-e2e-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

SyncObservation obs(std::string_view uri, std::string_view text, std::string_view author,
                    std::string_view created_at) {
    return SyncObservation{
        .text = std::string(text),
        .source_uri = std::string(uri),
        .author_did = std::string(author),
        .created_at = std::string(created_at),
    };
}

ScriptedFeed::ScriptedFeed(std::vector<ScriptedPage> pages) : pages_(std::move(pages)) {}

SyncPage ScriptedFeed::operator()(const std::optional<std::string> &cursor) {
    const std::size_t index = !cursor ? 0u : static_cast<std::size_t>(std::stoul(*cursor));
    if (index >= pages_.size()) {
        return SyncPage{}; /* exhausted */
    }
    const ScriptedPage &page = pages_[index];
    if (page.failure) {
        throw std::runtime_error("scripted transport failure");
    }
    return SyncPage{.items = page.items, .next_cursor = page.next_cursor};
}

Scenario::Scenario(std::string_view tag)
    : dir(scratch_dir(tag)), ledger_file(dir / "ledger.bin"), model_file(dir / "graph.snap"),
      state_file(dir / "state.json") {}

SyncResult Scenario::run(const SyncPageFetcher &feed, int max_pages, bool crash_after_sync) {
    LanguageGraph graph =
        std::filesystem::exists(model_file) ? LanguageGraph::load(model_file) : LanguageGraph();
    Ledger ledger(ledger_file);
    auto state = load_ingestion_state(state_file, "https://bsky.social", "did:plc:abc");

    SyncLimits limits;
    limits.max_pages = max_pages;
    const auto result = run_sync(graph, ledger, state, feed, limits);
    if (crash_after_sync) {
        return result; /* snapshot and state never saved */
    }
    graph.save(model_file);
    state.checkpoint.generation++;
    save_ingestion_state(state, state_file);
    return result;
}

Scenario::Cycle Scenario::run_cycle(const SyncPageFetcher &feed, int max_pages,
                                    bool crash_after_sync) {
    /* The gate is the daemon's: one durable read of the control file's
     * paused flag, from a cold open, before any ingestion work. A missing
     * control file is not paused — the fail-closed defaults leave the host
     * running and closed for writes. */
    if (load_control_state(control_file()).paused) {
        return Cycle{.result = SyncResult{}, .paused = true};
    }
    return Cycle{.result = run(feed, max_pages, crash_after_sync), .paused = false};
}

std::filesystem::path Scenario::journal_file() const {
    return dir / "action-journal.jsonl";
}
std::filesystem::path Scenario::control_file() const {
    return dir / "control-state.json";
}

std::string final_state_digest(const Scenario &scenario) {
    std::ostringstream out;
    if (std::filesystem::exists(scenario.model_file)) {
        std::ifstream input(scenario.model_file, std::ios::binary);
        out << input.rdbuf();
    }
    Ledger ledger(scenario.ledger_file);
    for (const auto &entry : ledger.entries()) {
        out << entry.id << '|' << entry.source_id << '|' << entry.author_did << '|'
            << entry.observed_at << '|' << entry.content_digest << '|' << entry.outcome << '\n';
    }
    return out.str();
}

} // namespace atperson::e2e
