#include "engine.hpp"

#include "state/time.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace atperson {

bool process_observation(LanguageGraph &graph, Ledger &ledger,
                          const SyncObservation &observation) {
    const std::uint64_t digest = Ledger::digest(observation.text);
    const std::uint64_t observed_at =
        parse_rfc3339_epoch(observation.created_at).value_or(0u);

    // Record the observation durably before training so a restarted process
    // can never re-train on it: the ledger is the authority for what has
    // already been committed. The canonical text is retained inline so the
    // ledger is replayable — a rebuild can recover the exact bytes — and the
    // conversational context rides in the record too, so a replay rebuild
    // restores reply/quote continuity as well.
    std::uint64_t id = 0u;
    const auto result = ledger.append(observation.source_uri, observation.author_did, observed_at,
                                      digest, ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING,
                                      observation.text, observation.context, &id);
    if (result == LedgerResult::ExistsCommitted) {
        return false;
    }

    const bool trainable = !observation.text.empty();
    const bool policy_skipped = observation.policy_reason != PolicyReason::Eligible &&
                                observation.policy_reason != PolicyReason::Repost &&
                                observation.policy_reason != PolicyReason::Reply;
    const auto outcome = trainable && !policy_skipped
                             ? ATP_LEDGER_OUTCOME_LEARNED
                             : ATP_LEDGER_OUTCOME_SKIPPED;
    if (trainable && !policy_skipped) {
        graph.remember(observation.text, observation.source_uri, observation.author_did,
                       observed_at, digest, ATPERSON_SCHEMA_VERSION, id);
    }
    ledger.set_outcome(id, outcome);

    atp_ledger_entry entry = {};
    entry.id = id;
    entry.observed_at = observed_at;
    entry.content_digest = digest;
    entry.schema_version = ATPERSON_SCHEMA_VERSION;
    entry.outcome = outcome;
    std::memcpy(entry.source_id, observation.source_uri.data(), observation.source_uri.size());
    entry.source_id[observation.source_uri.size()] = '\0';
    const std::size_t author_len = observation.author_did.size() < sizeof(entry.author_did) - 1u
                                       ? observation.author_did.size()
                                       : sizeof(entry.author_did) - 1u;
    std::memcpy(entry.author_did, observation.author_did.data(), author_len);
    entry.author_did[author_len] = '\0';
    graph.record_ledger_entry(entry, observation.context);
    return true;
}

SyncResult run_sync(LanguageGraph &graph, Ledger &ledger, IngestionState &state,
                    const SyncPageFetcher &fetch_page, const SyncLimits &limits,
                    const SyncLinker &link) {
    if (limits.page_size <= 0 || limits.max_pages <= 0) {
        throw std::runtime_error("sync limits must be positive");
    }

    SyncResult result;
    bool fresh_traversal = !state.catchup.active;

    if (fresh_traversal) {
        // A new traversal starts at the current timeline head; the ledger
        // filters anything already committed.
        state.checkpoint.pages_completed = 0u;
        state.checkpoint.observations_seen = 0u;
    }

    std::optional<std::string> cursor =
        state.catchup.active ? state.catchup.cursor : std::optional<std::string>{};

    int pages = 0;
    while (pages < limits.max_pages) {
        const SyncPage page = fetch_page(cursor);
        ++pages;

        // Process every observation in the page durably before considering
        // the page checkpointed. A throw from process_observation aborts the
        // run without advancing the persisted cursor; on restart the same
        // page is refetched and ledger dedup suppresses committed items.
        for (const auto &observation : page.items) {
            ++result.observations_seen;
            if (process_observation(graph, ledger, observation)) {
                /* #27: link the observation to any executed action it
                 * references, durably, before the page can be checkpointed.
                 * Duplicates (already-committed observations) skip linkage:
                 * their events were linked on first ingest. */
                if (link) {
                    link(observation);
                }
                const bool trainable = !observation.text.empty();
                const bool policy_skipped =
                    observation.policy_reason != PolicyReason::Eligible &&
                    observation.policy_reason != PolicyReason::Repost &&
                    observation.policy_reason != PolicyReason::Reply;
                if (trainable && !policy_skipped) {
                    result.learned++;
                } else {
                    result.skipped++;
                }
            } else {
                result.duplicates++;
            }
        }

        result.pages_completed++;

        if (!page.next_cursor) {
            // Exhausted: clear the cursor. The next independent sync begins
            // at the timeline head again; the ledger suppresses re-training.
            result.exhausted = true;
            break;
        }

        if (result.observations_seen >= limits.max_observations &&
            limits.max_observations > 0u) {
            // Observation budget reached mid-traversal: checkpoint the page
            // cursor so the next run resumes rather than restarting.
            cursor = page.next_cursor;
            break;
        }

        cursor = page.next_cursor;
    }

    // Persist the fetching checkpoint after the graph snapshot has been
    // saved by the caller (run order in main): the cursor never advances
    // past observations that have not been durably handled.
    if (result.exhausted) {
        state.catchup.active = false;
        state.catchup.cursor = std::nullopt;
    } else {
        state.catchup.active = true;
        state.catchup.cursor = cursor;
    }
    state.checkpoint.pages_completed += result.pages_completed;
    state.checkpoint.observations_seen += result.observations_seen;
    return result;
}

} // namespace atperson
