#ifndef ATPERSON_REPLICATE_PUBLISH_HPP
#define ATPERSON_REPLICATE_PUBLISH_HPP

// Bounded outbound publisher (#142): serialises committed ledger entries
// and journal entries to AT Protocol records under the entity's DID.
//
// The local ledger remains the commit authority for the learning loop —
// a network outage must never block or corrupt a local ledger commit. The
// publisher is a drain: it reads committed entries after the fact and
// pushes them to the network. If the network is down, the backlog stays
// local (the ledger already has the entries) and drains on reconnect.
//
// Progress is checkpointed in <data>/replicate/cursor.json: the next
// ledger id to publish and the next journal line to publish. The cursor
// advances only after a confirmed write, so a crash mid-drain resumes
// exactly where it stopped. putRecord is idempotent on (collection,
// rkey), so a retried overlap is safe.
//
// Withdrawal: a withdrawn entry republishes its observation record with
// outcome "withdrawn" (same rkey, putRecord update). The publisher
// re-checks outcomes of already-published entries each drain, bounded to
// a recheck window, so a withdrawal propagates without a full republish.

#include "atperson/ledger.hpp"
#include "outbound/execute.hpp"
#include "records.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace atperson {

class ReplicateCursorError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Durable drain progress. next_observation_id is the first ledger entry id
 * not yet published; actions_published/valence_published count journal
 * entries and thoughts_published counts thought-store entries already
 * pushed (all append-only, loaded in append order, so counts are stable
 * cursors). */
struct ReplicateCursor {
    std::uint64_t next_observation_id{1u};
    std::uint64_t actions_published{0u};
    std::uint64_t valence_published{0u};
    std::uint64_t thoughts_published{0u};
};

[[nodiscard]] ReplicateCursor load_replicate_cursor(const std::filesystem::path &path);
void save_replicate_cursor(const std::filesystem::path &path,
                           const ReplicateCursor &cursor);

/* One drain pass. Returns the report; throws only on cursor corruption.
 * Network errors surface as writer exceptions caught per record: the
 * drain stops at the first failure and the cursor stays at the last
 * confirmed write, so the backlog is retained. */
struct ReplicateReport {
    std::uint64_t observations_published{0u};
    std::uint64_t actions_published{0u};
    std::uint64_t valence_published{0u};
    std::uint64_t thoughts_published{0u};
    std::uint64_t withdrawal_updates{0u};
    bool network_failed{false};
    std::string failure_detail;
};

struct ReplicateConfig {
    /* Cap on records written per drain pass (rate/cost awareness: PDS
     * writes are not free; large backfills are chunked across passes). */
    std::uint32_t max_records_per_drain{50u};
    /* How far back (in entry ids) to recheck published entries for
     * outcome changes (withdrawal propagation), bounded. */
    std::uint32_t withdrawal_recheck_window{200u};
};

ReplicateReport replicate_drain(const std::filesystem::path &cursor_path,
                                const std::filesystem::path &journal_path, Ledger &ledger,
                                const std::filesystem::path &thoughts_path,
                                OutboundWriter &writer, const ReplicateConfig &config);

} // namespace atperson

#endif
