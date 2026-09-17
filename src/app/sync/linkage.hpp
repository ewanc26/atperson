#ifndef ATPERSON_SYNC_LINKAGE_HPP
#define ATPERSON_SYNC_LINKAGE_HPP

// Action-event linkage (#27): associate public replies and quotes with the
// executed outbound actions they reference, using stable identifiers only.
//
// During sync, every observation's conversational context (#24: reply parent,
// reply root, quote target at-URIs) is looked up against the executed
// actions in the durable journal. A match appends one `event` entry naming
// how the record referenced the action ("parent", "root" or "quote"). No
// text is ever matched: linkage is by at-URI, which is immutable once the
// record exists.
//
// Linkage records association only. It never interprets the event: mapping
// outcomes to value is the explicit #13 model, applied by the operator
// through the journal command, never here.
//
// Duplicate suppression: (action id, event uri) pairs already in the journal
// are skipped, so a restarted sync or a re-ingested page links each event
// exactly once. The check is against the journal as loaded at sync start;
// the caller holds the writer lock for the whole run, so no concurrent
// appends can race the dedup set.
//
// Failure modes: journal load/append I/O errors propagate as
// std::runtime_error. A linkage failure aborts the sync run before the page
// checkpoint advances, matching the engine's ordering invariant: an event
// is either durably linked or the page is retried.

#include "atperson/conversation.hpp"
#include "journal/store.hpp"

#include <filesystem>
#include <functional>
#include <string_view>

namespace atperson {

struct SyncObservation;

/* Action-event linkage sink (#27). The sync engine calls the linker once per
 * fetched observation, after the ledger commit and before the page
 * checkpoint, so an event is either durably linked or the page is retried.
 * A null linker disables linkage entirely (no journal on disk yet). */
using SyncLinker = std::function<void(const SyncObservation &observation)>;

/* How many events one observation can produce: at most one per referencing
 * field (parent, root, quote). */
struct LinkageResult {
    unsigned linked{0};    /* new event entries appended */
    unsigned duplicates{0}; /* matches already recorded */
};

/* Link one observation's context against `journal`. Appends at most one
 * event per referencing URI that resolves to an executed action. `at` is
 * the observation's own timestamp (RFC 3339); the event record's author and
 * URI come from the observation itself. */
LinkageResult link_observation(const std::filesystem::path &journal_path,
                               JournalContents &journal, std::string_view event_uri,
                               std::string_view author_did,
                               const ConversationContext &context, std::string_view at);

/* Build the sync-side linker over the journal at `journal_path`. The journal
 * is loaded once per run (a missing file disables linkage — there are no
 * executed actions to link) and the dedup set grows with each append, so a
 * re-ingested page links each event exactly once. The caller must hold the
 * writer lock for the whole run: the in-memory journal cannot race a
 * concurrent append. */
[[nodiscard]] SyncLinker make_journal_linker(const std::filesystem::path &journal_path);

} // namespace atperson

#endif
