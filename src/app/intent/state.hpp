#ifndef ATPERSON_INTENT_STATE_HPP
#define ATPERSON_INTENT_STATE_HPP

// Pending social intent derived state (#150): pure, deterministic logic over
// the journal's `intent` entries and the injected clock.
//
// The journal is append-only, so an intent's current record is the *last*
// entry with its id: opening appends one line, each continuation appends the
// same id with the conversation's actions grown by one, and the expiry sweep
// appends the terminal state. This module derives, for one intent id:
//   open     — the latest entry is `Open`, the window has not closed, and
//              the continuation budget is not exhausted;
//   expired  — the window closed with no continuation in time;
//   closed   — the continuation budget (max_continuations) was reached, so
//              the entity stops orbiting the thread.
// It also answers whether a ledger observation is a *continuation trigger*:
// the observation is the referencing record of a journal event whose action
// belongs to an open intent's conversation, from an acceptable responder.
//
// Everything here is read-only over `JournalContents` + `now` and never
// mutates anything. No network, no lock, no I/O.
//
// Failure semantics: an empty responder or id never matches. Only the last
// entry per id is consulted; earlier lines are history.

#include "journal/store.hpp"

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

namespace atperson {

/* The effective state of `latest` (the last entry with one intent id) at
 * `now`. Pure and deterministic. */
[[nodiscard]] IntentState derive_intent_state(const JournalIntent &latest, std::int64_t now);

/* The last journal entry with `id`, or nullptr when there is none. */
[[nodiscard]] const JournalIntent *latest_intent(const JournalContents &journal,
                                                 std::string_view id);

/* The currently active intents, one per id (the latest entry each), in
 * append order of their latest entries. Active = effective state Open. */
[[nodiscard]] std::vector<const JournalIntent *>
active_intents(const JournalContents &journal, std::int64_t now);

/* How many intents are currently active; the cap `record_pending_intent`
 * checks before opening a new conversation. */
[[nodiscard]] std::size_t active_intent_count(const JournalContents &journal, std::int64_t now);

/* The open intent whose conversation this observation continues, or nullptr.
 * `source_id`/`observed_at` are the candidate observation's identity and
 * instant; the observation counts when it is the referencing record of a
 * journal event (that event's `event_uri` is the observation), the event's
 * action belongs to an open intent's conversation, and the event's author is
 * an acceptable responder ("anyone" or the exact DID). Deterministic for
 * fixed journal and clock. */
[[nodiscard]] const JournalIntent *
continuation_intent(const JournalContents &journal, std::string_view source_id,
                    std::string_view author_did, std::int64_t now);

} // namespace atperson

#endif