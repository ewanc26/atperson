#ifndef ATPERSON_INTENT_SWEEP_HPP
#define ATPERSON_INTENT_SWEEP_HPP

// Pending social intent expiry sweep (#150): the idempotent pass that turns
// derived terminal intent states into durable journal lines.
//
// An intent's effective state is a pure function of its latest journal entry
// and the clock, but "expired" and "closed" must be *journaled* (expiry is
// durable and feeds #149's expectation resolution). This sweep appends, for
// every intent whose derived state is terminal, exactly one terminal entry —
// the id passed stays the same and the history records the knowledge change,
// mirroring the resolution pass. Replaying the sweep writes nothing new.
//
// Callers own locking exactly as for other journal mutations (the scheduler
// cycle runs it under the daemon's writer lock; tests drive it directly).
//
// Failure modes: journal I/O throws std::runtime_error; a corrupt journal
// throws JournalError through load_journal. The sweep never reopens a
// terminal intent and never shrinks an `actions` list.

#include "journal/store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace atperson {

struct IntentSweepReport {
    std::size_t evaluated{};       /* intent ids seen in the journal */
    std::size_t open_kept{};       /* still open: left alone */
    std::size_t expired_written{}; /* terminal lines appended this run */
    std::size_t closed_written{};  /* terminal lines appended this run */
    std::size_t already_terminal{}; /* latest entry already terminal: no write */
};

/* Run one idempotent sweep: append a terminal intent entry for every intent
 * whose derived state is terminal but whose latest entry is not yet. `now`
 * stamps the appended entries; `now_rfc3339` is the caller's pre-formatted
 * instant. Returns the count summary. */
[[nodiscard]] IntentSweepReport sweep_intents(const std::filesystem::path &journal_path,
                                              std::int64_t now,
                                              std::string_view now_rfc3339);

} // namespace atperson

#endif