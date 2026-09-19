/* Jetstream public backfill (#60) — engine-side driver.
 *
 * This translation unit is compiled only into the network runtime (the
 * `atperson` binary), because it drives the Wolfram-backed JetstreamClient.
 * It owns the per-cycle pipeline that funnels Jetstream commit frames into
 * the same durable ledger contract as the polling path: ledger reservation
 * (PENDING) -> remember -> outcome commit (LEARNED/SKIPPED) -> action-event
 * linkage. `process_observation` is the shared core; this driver only picks
 * the observations and checkpoints the cursor afterwards.
 *
 * The cursor is an opaque Jetstream sequence number stored as a decimal
 * string in the ingestion state. The C23 core never sees it; it is C++23
 * runtime metadata. After a bounded cycle it is persisted so the next
 * independent backfill resumes from the last processed frame; the durable
 * ledger suppresses anything already committed.
 *
 * Bounded work: each cycle processes at most `limits.max_events` commit
 * frames and stops after `limits.max_ms` of wall time, so a misbehaving feed
 * cannot starve the daemon. The cursor is checkpointed only after every event
 * in the cycle has been durably handled, and never when empty (a fresh feed
 * whose head has not produced a frame yet stays "not active": the next
 * backfill starts from wherever Jetstream resumes, which is the same as an
 * initial head start because the ledger deduplicates).
 */

#include "engine.hpp"
#include "atproto/jetstream_client.hpp"

namespace atperson {

namespace {

/* Convert one decoded Jetstream frame into the engine's observation shape.
 * The translation is trivial: the client already ran the offline-testable
 * extractor, so this is a pure copy of the fields the engine needs. */
SyncObservation jetstream_to_observation(const JetstreamEvent &event) {
    SyncObservation observation;
    observation.text = event.text;
    observation.source_uri = event.source_uri;
    observation.author_did = event.author_did;
    observation.created_at = event.created_at;
    observation.context.reply_root_uri = event.reply_root;
    observation.context.reply_parent_uri = event.reply_parent;
    observation.context.quote_uri = event.quote_uri;
    if (event.text.empty()) {
        observation.policy_reason = PolicyReason::EmptyText;
    } else if (!event.quote_uri.empty()) {
        observation.policy_reason = PolicyReason::Quote;
    } else {
        observation.policy_reason = PolicyReason::Eligible;
    }
    return observation;
}

} // namespace

JetstreamRunResult run_jetstream_backfill(LanguageGraph &graph, Ledger &ledger,
                                          IngestionState &state, JetstreamClient &client,
                                          const JetstreamLimits &limits,
                                          const SyncLinker &link, std::string_view self_did) {
    JetstreamRunResult result;
    const bool fresh_traversal = !state.catchup.active;

    if (fresh_traversal) {
        state.checkpoint.pages_completed = 0u;
        state.checkpoint.observations_seen = 0u;
    }

    const auto on_event = [&](const JetstreamEvent &event) {
        if (event.deleted) {
            result.withdrawn += ledger.withdraw_source(event.source_uri);
            return;
        }
        SyncObservation observation = jetstream_to_observation(event);
        if (!self_did.empty() && event.author_did == self_did) {
            observation.policy_reason = PolicyReason::SelfAuthored;
        }
        ++result.observations_seen;
        if (process_observation(graph, ledger, observation)) {
            if (link) {
                link(observation);
            }
            const bool trainable = !observation.text.empty();
            const bool policy_skipped =
                observation.policy_reason != PolicyReason::Eligible &&
                observation.policy_reason != PolicyReason::Repost &&
                observation.policy_reason != PolicyReason::Reply &&
                observation.policy_reason != PolicyReason::Quote;
            if (trainable && !policy_skipped) {
                ++result.learned;
            } else {
                ++result.skipped;
            }
        } else {
            ++result.duplicates;
        }
    };

    const auto batch = client.fetch_batch(limits, on_event);
    result.events_consumed = batch.first;
    result.exhausted = batch.second;

    if (result.exhausted) {
        state.catchup.active = false;
        state.catchup.cursor = std::nullopt;
    } else {
        const std::string cursor = client.cursor();
        state.catchup.active = !cursor.empty();
        state.catchup.cursor = cursor.empty() ? std::optional<std::string>{}
                                              : std::optional<std::string>(cursor);
    }
    state.checkpoint.pages_completed += 1u;
    state.checkpoint.observations_seen += result.observations_seen;
    return result;
}

} // namespace atperson
