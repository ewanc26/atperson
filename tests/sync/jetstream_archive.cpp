#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "atproto/jetstream_replay_client.hpp"
#include "engine.hpp"
#include "ingestion/state.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class ScriptedReplay final : public atperson::JetstreamReplaySource {
  public:
    [[nodiscard]] atperson::JetstreamReplayWindow fetch_window(
        std::uint64_t after, std::optional<std::uint64_t> before,
        std::string_view, const std::vector<std::string> &, const std::vector<std::string> &,
        const std::function<void(const atperson::JetstreamEvent &)> &on_event) override {
        after_values.push_back(after);
        before_values.push_back(before);
        if (calls++ == 0u) {
            on_event(atperson::JetstreamEvent{
                .source_uri = "at://did:plc:archive/app.bsky.feed.post/one",
                .author_did = "did:plc:author",
                .created_at = "2026-09-20T00:00:00Z",
                .text = "first archive event",
            });
            return {.planned_through_seq = 50u, .sealed_tip_seq = 100u};
        }
        return {.planned_through_seq = 100u, .sealed_tip_seq = 100u};
    }

    std::vector<std::uint64_t> after_values;
    std::vector<std::optional<std::uint64_t>> before_values;
    unsigned calls{};
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "atperson-archive-restart-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto state_path = root / "state.json";
    const auto model_path = root / "model.bin";
    const auto ledger_path = root / "ledger.bin";

    atperson::LanguageGraph graph;
    atperson::Ledger ledger(ledger_path);
    auto state = atperson::initial_ingestion_state(
        "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    ScriptedReplay replay;
    const std::vector<std::string> collections{"app.bsky.feed.post"};
    const std::vector<std::string> dids;

    const auto first = atperson::run_jetstream_archive(
        graph, ledger, state, replay, 0u, 100u, "did:plc:self", collections, dids);
    assert(!first.exhausted);
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("50"));
    graph.save(model_path);
    atperson::save_ingestion_state(state, state_path);

    graph = atperson::LanguageGraph::load(model_path);
    state = atperson::load_ingestion_state(
        state_path, "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    const auto second = atperson::run_jetstream_archive(
        graph, ledger, state, replay, 50u, 100u, "did:plc:self", collections, dids);
    assert(second.exhausted);
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("100"));
    assert(replay.after_values == std::vector<std::uint64_t>({0u, 50u}));
    assert(replay.before_values[0] == std::optional<std::uint64_t>(100u));
    assert(replay.before_values[1] == std::optional<std::uint64_t>(100u));
    std::filesystem::remove_all(root);
    return 0;
}
