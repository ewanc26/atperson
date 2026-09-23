#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "atproto/jetstream_replay_client.hpp"
#include "engine.hpp"
#include "ingestion/state.hpp"
#include "protocol.hpp"

#include <cassert>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class ScriptedReplay final : public atperson::JetstreamReplaySource {
  public:
    [[nodiscard]] std::optional<std::uint64_t> probe_sealed_tip() override {
        return sealed_tip;
    }

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
                .seq = 42,
                .repo_revision = "3jui3s7xq2m2a",
            });
            return {.planned_through_seq = 50u, .sealed_tip_seq = 100u};
        }
        return {.planned_through_seq = 100u, .sealed_tip_seq = 100u};
    }

    std::vector<std::uint64_t> after_values;
    std::vector<std::optional<std::uint64_t>> before_values;
    std::uint64_t sealed_tip{100u};
    unsigned calls{};
};

class DeletionReplay final : public atperson::JetstreamReplaySource {
  public:
    [[nodiscard]] std::optional<std::uint64_t> probe_sealed_tip() override {
        return sealed_tip;
    }

    [[nodiscard]] atperson::JetstreamReplayWindow fetch_window(
        std::uint64_t, std::optional<std::uint64_t>, std::string_view,
        const std::vector<std::string> &, const std::vector<std::string> &,
        const std::function<void(const atperson::JetstreamEvent &)> &on_event) override {
        const std::string source = "at://did:plc:archive/app.bsky.feed.post/withdrawn";
        if (calls++ == 0u) {
            on_event(atperson::JetstreamEvent{
                .source_uri = source,
                .author_did = "did:plc:author",
                .created_at = "2026-09-20T00:00:00Z",
                .text = "archive event to withdraw",
            });
        } else {
            on_event(atperson::JetstreamEvent{.source_uri = source, .deleted = true});
        }
        return {.planned_through_seq = calls * 10u, .sealed_tip_seq = calls * 10u};
    }

    std::uint64_t sealed_tip{100u};
    unsigned calls{};
};

class FailingReplay final : public atperson::JetstreamReplaySource {
  public:
    [[nodiscard]] std::optional<std::uint64_t> probe_sealed_tip() override {
        return sealed_tip;
    }

    [[nodiscard]] atperson::JetstreamReplayWindow fetch_window(
        std::uint64_t, std::optional<std::uint64_t>, std::string_view,
        const std::vector<std::string> &, const std::vector<std::string> &,
        const std::function<void(const atperson::JetstreamEvent &)> &on_event) override {
        on_event(atperson::JetstreamEvent{
            .source_uri = "at://did:plc:archive/app.bsky.feed.post/failure",
            .author_did = "did:plc:author",
            .created_at = "2026-09-20T00:00:00Z",
            .text = "event before transport failure",
        });
        throw std::runtime_error("scripted replay transport failure");
    }

    std::uint64_t sealed_tip{100u};
};

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "atperson-archive-restart-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto state_path = root / "state.json";
    const auto model_path = root / "model.bin";
    const auto ledger_path = root / "ledger.bin";
    const auto protocol_path = root / "protocol.bin";

    atperson::LanguageGraph graph;
    atperson::Ledger ledger(ledger_path);
    atperson::protocol::EvidenceLedger protocol_ledger(protocol_path);
    auto state = atperson::initial_ingestion_state(
        "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    ScriptedReplay replay;
    const std::vector<std::string> collections{"app.bsky.feed.post"};
    const std::vector<std::string> dids;

    const auto first = atperson::run_jetstream_archive(
        graph, ledger, state, replay, 0u, 100u, std::nullopt, "did:plc:self",
        collections, dids, nullptr, &protocol_ledger);
    assert(!first.exhausted);
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("50"));
    graph.save(model_path);
    atperson::save_ingestion_state(state, state_path);

    graph = atperson::LanguageGraph::load(model_path);
    state = atperson::load_ingestion_state(
        state_path, "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    const auto second = atperson::run_jetstream_archive(
        graph, ledger, state, replay, 50u, 100u, std::nullopt, "did:plc:self",
        collections, dids, nullptr, &protocol_ledger);
    assert(second.exhausted);
    assert(state.catchup.active);
    assert(state.catchup.cursor == std::optional<std::string>("100"));
    assert(replay.after_values == std::vector<std::uint64_t>({0u, 50u}));
    assert(replay.before_values[0] == std::optional<std::uint64_t>(100u));
    assert(replay.before_values[1] == std::optional<std::uint64_t>(100u));
    const auto protocol_entries = protocol_ledger.entries();
    assert(protocol_entries.size() == 1u);
    assert(protocol_entries.front().event_type == "#commit");
    assert(protocol_entries.front().source == "jetstream-archive");
    assert(protocol_entries.front().payload.find("|42|3jui3s7xq2m2a") != std::string::npos);

    const auto withdrawal_root =
        std::filesystem::temp_directory_path() / "atperson-archive-withdrawal-test";
    std::filesystem::remove_all(withdrawal_root);
    std::filesystem::create_directories(withdrawal_root);
    atperson::LanguageGraph withdrawal_graph;
    atperson::Ledger withdrawal_ledger(withdrawal_root / "ledger.bin");
    atperson::IngestionState withdrawal_state = atperson::initial_ingestion_state(
        "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    DeletionReplay withdrawal_replay;
    const auto ingested = atperson::run_jetstream_archive(
        withdrawal_graph, withdrawal_ledger, withdrawal_state, withdrawal_replay, 0u,
        10u, std::nullopt, "did:plc:self", collections, dids);
    assert(ingested.learned == 1u);
    const auto withdrawn = atperson::run_jetstream_archive(
        withdrawal_graph, withdrawal_ledger, withdrawal_state, withdrawal_replay, 10u,
        20u, std::nullopt, "did:plc:self", collections, dids);
    assert(withdrawn.withdrawn == 1u);
    assert(withdrawn.reconciled);
    std::filesystem::remove_all(withdrawal_root);

    atperson::LanguageGraph failure_graph;
    const auto failure_root =
        std::filesystem::temp_directory_path() / "atperson-archive-failure-test";
    std::filesystem::remove_all(failure_root);
    std::filesystem::create_directories(failure_root);
    atperson::Ledger failure_ledger(failure_root / "ledger.bin");
    atperson::IngestionState failure_state = atperson::initial_ingestion_state(
        "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    failure_state.catchup.active = true;
    failure_state.catchup.cursor = std::string("40");
    FailingReplay failing_replay;
    bool failed = false;
    try {
        static_cast<void>(atperson::run_jetstream_archive(
            failure_graph, failure_ledger, failure_state, failing_replay, 40u, 50u,
            std::nullopt, "did:plc:self", collections, dids));
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);
    assert(failure_state.catchup.active);
    assert(failure_state.catchup.cursor == std::optional<std::string>("40"));
    std::filesystem::remove_all(failure_root);

    const auto relative_root =
        std::filesystem::temp_directory_path() / "atperson-archive-relative-test";
    std::filesystem::remove_all(relative_root);
    std::filesystem::create_directories(relative_root);
    atperson::LanguageGraph relative_graph;
    atperson::Ledger relative_ledger(relative_root / "ledger.bin");
    atperson::IngestionState relative_state = atperson::initial_ingestion_state(
        "wss://jetstream.example/subscribe", "", atperson::kSourceKindJetstream);
    ScriptedReplay relative_replay;
    relative_replay.sealed_tip = 100u;
    const auto relative_ingested = atperson::run_jetstream_archive(
        relative_graph, relative_ledger, relative_state, relative_replay, 0u,
        std::nullopt, std::optional<std::uint64_t>(50u), "did:plc:self",
        collections, dids);
    assert(relative_ingested.events_consumed == 1u);
    assert(relative_ingested.learned == 1u);
    assert(relative_replay.after_values.front() == 50u);
    assert(relative_replay.before_values.front() == std::optional<std::uint64_t>(100u));

    ScriptedReplay whole_archive_replay;
    whole_archive_replay.sealed_tip = 100u;
    const auto whole_archive = atperson::run_jetstream_archive(
        relative_graph, relative_ledger, relative_state, whole_archive_replay, 0u,
        std::nullopt, std::optional<std::uint64_t>(500u), "did:plc:self",
        collections, dids);
    assert(whole_archive.events_consumed == 1u);
    assert(whole_archive_replay.after_values.front() == 0u);
    assert(whole_archive_replay.before_values.front() == std::optional<std::uint64_t>(100u));

    ScriptedReplay explicit_before_replay;
    explicit_before_replay.sealed_tip = 100u;
    const auto explicit_before = atperson::run_jetstream_archive(
        relative_graph, relative_ledger, relative_state, explicit_before_replay, 0u,
        std::optional<std::uint64_t>(80u), std::optional<std::uint64_t>(30u),
        "did:plc:self", collections, dids);
    assert(explicit_before.events_consumed == 1u);
    assert(explicit_before_replay.after_values.front() == 50u);
    assert(explicit_before_replay.before_values.front() == std::optional<std::uint64_t>(80u));

    ScriptedReplay empty_tip_replay;
    empty_tip_replay.sealed_tip = 0u;
    bool empty_refused = false;
    try {
        static_cast<void>(atperson::run_jetstream_archive(
            relative_graph, relative_ledger, relative_state, empty_tip_replay, 0u,
            std::nullopt, std::optional<std::uint64_t>(50u), "did:plc:self",
            collections, dids));
    } catch (const std::runtime_error &) {
        empty_refused = true;
    }
    assert(empty_refused);

    bool cap_refused = false;
    try {
        static_cast<void>(atperson::run_jetstream_archive(
            relative_graph, relative_ledger, relative_state, whole_archive_replay, 0u,
            std::nullopt, std::optional<std::uint64_t>(10'000'000u + 1u),
            "did:plc:self", collections, dids));
    } catch (const std::runtime_error &) {
        cap_refused = true;
    }
    assert(cap_refused);
    std::filesystem::remove_all(relative_root);

    std::filesystem::remove_all(root);
    return 0;
}
