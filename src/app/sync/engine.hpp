#ifndef ATPERSON_SYNC_ENGINE_HPP
#define ATPERSON_SYNC_ENGINE_HPP

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "policy.hpp"
#include "ingestion/state.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

/* One fetched feed item, already reduced to what the learning core needs.
 * Produced by Wolfram-backed AtprotoClient in the network build and by test
 * fixtures offline. `policy_reason` records why the item was skipped or why
 * it is eligible (eligible/repost/reply); skipped items carry empty text and
 * are committed to the ledger as SKIPPED, never trained on. `context` is
 * planning metadata: it rides with the observation but never enters the
 * learned text. */
struct SyncObservation {
    std::string text;
    std::string source_uri;
    std::string author_did;
    std::string created_at;
    PolicyReason policy_reason{PolicyReason::Eligible};
    ConversationContext context;
};

/* One page of feed items plus the opaque cursor for the next page.
 * `next_cursor` is nullopt when the traversal is exhausted. */
struct SyncPage {
    std::vector<SyncObservation> items;
    std::optional<std::string> next_cursor;
};

/* Page fetcher. Returns the next page; throws on transport failure. */
using SyncPageFetcher = std::function<SyncPage(const std::optional<std::string> &cursor)>;

/* Bounded traversal budget. */
struct SyncLimits {
    int page_size{50};
    int max_pages{1};
    std::uint64_t max_observations{0}; /* 0 = unbounded */
};

/* Per-run accounting, printed by the CLI and asserted in tests. `skipped`
 * counts policy-skipped items (empty text, self-authored, blocked, …);
 * `duplicates` counts items the ledger already had. */
struct SyncResult {
    std::uint64_t pages_completed{};
    std::uint64_t observations_seen{};
    std::size_t learned{};
    std::size_t remembered{};
    std::size_t skipped{};
    std::size_t duplicates{};
    bool exhausted{}; /* traversal reached the end of the timeline */
};

/*
 * Process one feed item through the durable pipeline: ledger reservation
 * (PENDING) -> remember -> outcome commit -> snapshot mirror. Returns false
 * when the item was already durably committed (ledger dedup). Throws only
 * on genuine persistence failure; a throw means the item is left retryable.
 */
bool process_observation(LanguageGraph &graph, Ledger &ledger,
                         const SyncObservation &observation);

/*
 * Bounded multi-page catch-up traversal.
 *
 * Ordering invariant: every observation in a page is processed durably before
 * the page's cursor is checkpointed. A mid-page failure aborts without
 * advancing the persisted cursor, so the same page is refetched on restart and
 * ledger deduplication suppresses anything already committed.
 *
 * The ledger remains the authority for what has been learned; the cursor
 * only controls fetching progress.
 */
SyncResult run_sync(LanguageGraph &graph, Ledger &ledger, IngestionState &state,
                    const SyncPageFetcher &fetch_page, const SyncLimits &limits);

} // namespace atperson

#endif
