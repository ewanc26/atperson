#ifndef ATPERSON_SYNC_ENGINE_HPP
#define ATPERSON_SYNC_ENGINE_HPP

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "protocol.hpp"
#include "policy.hpp"
#include "ingestion/state.hpp"
#include "linkage.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

class JetstreamClient; /* defined in atproto/jetstream_client.hpp */
class JetstreamReplaySource;
class JetstreamReplayClient;

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
 * ledger deduplication suppresses anything already committed. Action-event
 * linkage (#27) runs under the same invariant: `link` fires per observation
 * after its ledger commit, and a linkage failure aborts the page like any
 * other persistence failure.
 *
 * The ledger remains the authority for what has been learned; the cursor
 * only controls fetching progress.
 */
SyncResult run_sync(LanguageGraph &graph, Ledger &ledger, IngestionState &state,
                    const SyncPageFetcher &fetch_page, const SyncLimits &limits,
                    const SyncLinker &link = nullptr);

/* Jetstream public backfill (#60): the unauthenticated feed path. Each cycle
 * fetches a bounded batch of commit frames through `client`, translates each
 * one through the same pipeline as `run_sync` (ledger reservation -> remember
 * -> outcome commit -> linkage), and checkpoints the Jetstream cursor only
 * after every event in the batch has been durably handled.
 *
 * The cursor is an opaque Jetstream sequence number; the engine stores it as
 * a decimal string in the ingestion state and never parses it. After a
 * bounded cycle the cursor is persisted and the next independent backfill
 * resumes from it, relying on the durable ledger for deduplication.
 *
 * Failure modes: throws on a fatal client/transport error. Malformed
 * Jetstream frames are bounded, counted and skipped by the client. A WOULD_BLOCK return from
 * `JetstreamClient::fetch_batch` (reconnect backoff) is not a failure: the
 * caller sleeps for the advertised delay and retries the same batch. */
struct JetstreamLimits {
    std::uint64_t max_events{0}; /* 0 = unbounded */
    std::int64_t max_ms{0};      /* 0 = unbounded */
};

struct JetstreamRunResult {
    std::uint64_t events_consumed{};
    std::uint64_t malformed_frames{};
    std::uint64_t observations_seen{};
    std::size_t learned{};
    std::size_t skipped{};
    std::size_t duplicates{};
    std::size_t withdrawn{};
    bool reconciled{};
    bool protocol_resync_required{};
    bool exhausted{}; /* feed closed cleanly at the head */
};

struct JetstreamResyncResult {
    std::uint64_t sequence{};
    std::string repo;
    std::string revision;
    protocol::Verification verification{protocol::Verification::Unverified};
};

using JetstreamResyncExecutor = std::function<std::optional<JetstreamResyncResult>(
    const protocol::ResyncPlan &plan)>;

/* Implemented in sync/jetstream_backfill.cpp, which is compiled only into
 * the network runtime (it drives the Wolfram-backed JetstreamClient). The
 * engine declares it here so the daemon loop and CLI share the contract. */
JetstreamRunResult run_jetstream_backfill(LanguageGraph &graph, Ledger &ledger,
                                          IngestionState &state,
                                          JetstreamClient &client,
                                          const JetstreamLimits &limits,
                                          const SyncLinker &link = nullptr,
                                          protocol::EvidenceLedger *protocol_ledger = nullptr,
                                          const JetstreamResyncExecutor &resync = nullptr);

/* Process one bounded sealed-archive window through the same durable pipeline
 * as live Jetstream. On success, the persisted Jetstream cursor is set to the
 * sealed tip so the next live cycle resumes after the archive without a gap.
 *
 * An optional relative_span treats the window as the trailing <span> sequences
 * ending at before_seq (or, when before_seq is absent, at the sealed archive
 * tip discovered through client.probe_sealed_tip()). The span is hard-capped
 * by kJetstreamArchiveMaxSequenceSpan; the caller supplies either an explicit
 * after_seq or a relative_span, never both.
 */
JetstreamRunResult run_jetstream_archive(LanguageGraph &graph, Ledger &ledger,
                                         IngestionState &state,
                                         JetstreamReplaySource &client,
                                         std::uint64_t after_seq,
                                         std::optional<std::uint64_t> before_seq,
                                         std::optional<std::uint64_t> relative_span,
                                         std::string_view self_did,
                                         const std::vector<std::string> &collections,
                                         const std::vector<std::string> &dids,
                                         const SyncLinker &link = nullptr,
                                         protocol::EvidenceLedger *protocol_ledger = nullptr);

} // namespace atperson

#endif
