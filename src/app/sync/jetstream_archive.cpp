#include "engine.hpp"

#include "atproto/jetstream_replay_client.hpp"

namespace atperson {

JetstreamRunResult run_jetstream_archive(
    LanguageGraph &graph, Ledger &ledger, IngestionState &state,
    JetstreamReplaySource &client, std::uint64_t after_seq,
    std::optional<std::uint64_t> before_seq, std::string_view self_did,
    const std::vector<std::string> &collections,
    const std::vector<std::string> &dids,
    const SyncLinker &link, protocol::EvidenceLedger *protocol_ledger) {
    JetstreamRunResult result;
    const auto on_event = [&](const JetstreamEvent &event) {
        ++result.events_consumed;
        if (protocol_ledger != nullptr) {
            (void)protocol::append_firehose_event(
                *protocol_ledger, "jetstream-archive",
                event.deleted ? "#commit/delete" : event.event_type,
                event.author_did, event.source_uri + "|" + std::to_string(event.seq),
                event.seq, 0u);
        }
        if (event.deleted) {
            result.withdrawn += ledger.withdraw_source(event.source_uri);
            return;
        }
        SyncObservation observation;
        observation.text = event.text;
        observation.source_uri = event.source_uri;
        observation.author_did = event.author_did;
        observation.created_at = event.created_at;
        observation.context.reply_root_uri = event.reply_root;
        observation.context.reply_parent_uri = event.reply_parent;
        observation.context.quote_uri = event.quote_uri;
        observation.policy_reason = event.policy_reason;
        ++result.observations_seen;
        if (process_observation(graph, ledger, observation)) {
            if (link) link(observation);
            if (!observation.text.empty() &&
                policy_reason_is_eligible(observation.policy_reason)) {
                ++result.learned;
            } else {
                ++result.skipped;
            }
        } else {
            ++result.duplicates;
        }
    };

    const auto window = client.fetch_window(
        after_seq, before_seq, self_did, collections, dids, on_event);
    result.exhausted = window.planned_through_seq >= window.sealed_tip_seq;
    if (result.exhausted) {
        /* Jetstream's live cursor uses the same monotonic sequence space. */
        state.catchup.active = window.sealed_tip_seq != 0u;
        state.catchup.cursor = state.catchup.active
                                   ? std::optional<std::string>(
                                         std::to_string(window.sealed_tip_seq))
                                   : std::nullopt;
    } else {
        state.catchup.active = true;
        state.catchup.cursor = std::to_string(window.planned_through_seq);
    }
    if (result.withdrawn > 0u) {
        (void)graph.rebuild_from_ledger(ledger);
        result.reconciled = true;
    }
    state.checkpoint.pages_completed++;
    state.checkpoint.observations_seen += result.observations_seen;
    return result;
}

} // namespace atperson
