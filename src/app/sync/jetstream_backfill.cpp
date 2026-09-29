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

#include <charconv>

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
    observation.policy_reason = event.policy_reason;
    return observation;
}

} // namespace

JetstreamRunResult run_jetstream_backfill(LanguageGraph &graph, Ledger &ledger,
                                          IngestionState &state, JetstreamClient &client,
                                          const JetstreamLimits &limits,
                                          const SyncLinker &link,
                                          protocol::EvidenceLedger *protocol_ledger,
                                          const JetstreamResyncExecutor &resync) {
    JetstreamRunResult result;
    protocol::CursorState protocol_cursor;
    if (state.catchup.cursor) {
        std::uint64_t persisted = 0;
        const auto parsed = std::from_chars(
            state.catchup.cursor->data(),
            state.catchup.cursor->data() + state.catchup.cursor->size(), persisted);
        if (parsed.ec == std::errc{} &&
            parsed.ptr == state.catchup.cursor->data() + state.catchup.cursor->size())
            protocol_cursor.last_sequence = persisted;
    }
    if (state.catchup.protocol_repo && state.catchup.protocol_revision) {
        protocol_cursor.repo = *state.catchup.protocol_repo;
        protocol_cursor.repo_revision = *state.catchup.protocol_revision;
        protocol_cursor.repository_revisions.push_back(
            {protocol_cursor.repo, protocol_cursor.repo_revision});
    }
    const bool fresh_traversal = !state.catchup.active;

    if (fresh_traversal) {
        state.checkpoint.pages_completed = 0u;
        state.checkpoint.observations_seen = 0u;
    }

    /* A collection- or DID-filtered subscription sees a subset of Jetstream's
     * global sequence, so its jumps are expected: the cursor advances across
     * them instead of demanding a resync (which nothing could ever clear).
     * They are still reconciliation signals, so they are counted and recorded
     * below as unverified evidence; they never mark anything verified and
     * never discard the valid event. An unfiltered stream stays strictly
     * contiguous, and a real gap there still requires resync. */
    const bool contiguous = !client.filtered();
    std::size_t sequence_jumps = 0u;
    std::uint64_t first_jump_after = 0u;
    std::uint64_t last_jump_to = 0u;

    const auto on_event = [&](const JetstreamEvent &event) {
        const std::uint64_t event_sequence =
            event.seq > 0 ? static_cast<std::uint64_t>(event.seq) : 0u;
        if (!contiguous && event_sequence != 0u && protocol_cursor.last_sequence != 0u &&
            !protocol_cursor.resync_required &&
            event_sequence > protocol_cursor.last_sequence + 1u) {
            if (sequence_jumps == 0u) {
                first_jump_after = protocol_cursor.last_sequence;
            }
            ++sequence_jumps;
            last_jump_to = event_sequence;
        }
        const auto cursor_result = event.protocol_only
            ? protocol::observe_sequence(protocol_cursor, event_sequence, contiguous)
            : event.repo_revision.empty()
                  ? protocol::observe_sequence(protocol_cursor, event_sequence, contiguous)
                  : protocol::observe_stream(protocol_cursor, event_sequence,
                                             event.author_did, event.repo_revision,
                                             contiguous);
        if (cursor_result == protocol::CursorResult::Gap) {
            /* Unfiltered stream: commits were lost. Request reconciliation; the
             * valid event is still kept. */
            result.protocol_resync_required = true;
        }
        if (cursor_result == protocol::CursorResult::Rewind ||
            cursor_result == protocol::CursorResult::Rejected) {
            result.protocol_resync_required = true;
            if (protocol_ledger != nullptr) {
                (void)protocol::append_firehose_event(
                    *protocol_ledger, "jetstream", "#cursor-gap", event.author_did,
                    event.repo_revision,
                    event.seq > 0 ? static_cast<std::uint64_t>(event.seq) : 0u, 0u,
                    cursor_result == protocol::CursorResult::Rejected
                        ? protocol::Verification::Rejected
                        : protocol::Verification::Unverified);
            }
            return;
        }
        if (!event.protocol_only && !protocol_cursor.repo.empty()) {
            state.catchup.protocol_repo = protocol_cursor.repo;
            state.catchup.protocol_revision = protocol_cursor.repo_revision;
        }
        if (protocol_ledger != nullptr &&
            (limits.record_commit_evidence || event.deleted || event.event_type != "#commit")) {
            (void)protocol::append_firehose_event(
                *protocol_ledger, "jetstream",
                event.deleted ? "#commit/delete" : event.event_type,
                event.author_did,
                event.protocol_only
                    ? event.protocol_payload
                    : event.source_uri + "|" + std::to_string(event.seq) +
                          "|" + event.repo_revision,
                event.seq > 0 ? static_cast<std::uint64_t>(event.seq) : 0u, 0u,
                event.verification);
        }
        if (event.protocol_only) return;
        if (event.deleted) {
            result.withdrawn += ledger.withdraw_source(event.source_uri);
            return;
        }
        const SyncObservation observation = jetstream_to_observation(event);
        ++result.observations_seen;
        if (process_observation(graph, ledger, observation)) {
            if (link) {
                link(observation);
            }
            const bool trainable = !observation.text.empty();
            const bool policy_skipped =
                !policy_reason_is_eligible(observation.policy_reason);
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
    result.events_consumed = batch.frames_consumed;
    result.malformed_frames = batch.malformed_frames;
    result.exhausted = batch.exhausted;
    result.filtered_sequence_jumps = sequence_jumps;
    if (sequence_jumps != 0u && protocol_ledger != nullptr) {
        /* One aggregate record per batch keeps the signal durable without an
         * evidence entry for every skipped sequence on a busy filtered stream. */
        (void)protocol::append_firehose_event(
            *protocol_ledger, "jetstream", "#filtered-sequence-jump", "",
            "jumps=" + std::to_string(sequence_jumps) +
                "|first_after=" + std::to_string(first_jump_after) +
                "|last_to=" + std::to_string(last_jump_to),
            last_jump_to, 0u, protocol::Verification::Unverified);
    }

    if (result.protocol_resync_required && resync) {
        const auto plan = protocol::plan_resync(protocol_cursor, 0u);
        if (plan.required) {
            const auto recovered = resync(plan);
            if (recovered && protocol::complete_resync(
                                 protocol_cursor, recovered->sequence,
                                 recovered->repo, recovered->revision,
                                 recovered->verification)) {
                result.protocol_resync_required = false;
                state.catchup.active = true;
                state.catchup.cursor = std::to_string(recovered->sequence);
                state.catchup.protocol_repo = recovered->repo;
                state.catchup.protocol_revision = recovered->revision;
            }
        }
    }

    if (result.withdrawn > 0u) {
        (void)graph.rebuild_from_ledger(ledger);
        result.reconciled = true;
    }

    if (result.protocol_resync_required) {
        /* Never checkpoint past a protocol gap. The persisted cursor remains
         * the last known-good boundary until a validated CAR resync clears it. */
        state.catchup.active = true;
    } else if (result.exhausted) {
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
