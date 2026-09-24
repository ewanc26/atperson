#ifndef ATPERSON_JOURNAL_RESOLVE_HPP
#define ATPERSON_JOURNAL_RESOLVE_HPP

// Pre-action expectation resolution (#149): mark an executed action's
// recorded prediction `met`, `unmet` or `expired` from the journal events
// linked to it.
//
// An executed action whose decision recorded an expectation is judged
// against the events (replies/quotes) that later referenced its record:
//   met      — at least one linked event landed before the window closed;
//   unmet    — a linked event exists but none landed in time (a late
//              reference);
//   expired  — the window closed with no linked event;
//   pending  — derived, no events yet and the window is still open;
//   none     — no expectation recorded, or not an executed action.
// The state is a pure function of the journal and the injected `now`, so it
// is deterministic and re-derivable on reconstruct. Injective literals are
// not special-cased: the event's Unix instant is compared, and an event
// whose timestamp fails to parse counts as contact that did not land in
// time.
//
// `resolve_expectations` persists only *terminal* states as journal
// resolution lines, idempotently: replaying the pass never duplicates an
// already-recorded state. A state change (an expired action later gains a
// late reference and becomes unmet) appends the new terminal state, so the
// line history records the knowledge change while the derived state remains
// the source of truth. The function never locks — callers hold the writer
// lock exactly as for other journal mutations.
//
// Failure modes: journal I/O on the pass throws std::runtime_error; a
// corrupt journal throws JournalError through load_journal. A terminal
// state with no prior line is written; everything else is only counted.

#include "store.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace atperson {

/* Bounded window for expectation resolution, in seconds: a linked event
 * must land within this after the attempt to count as `met`. Mirrors the
 * reciprocity window drives use (#148): one week. */
inline constexpr std::int64_t kExpectationWindowSeconds = 7 * 24 * 3600;

/* The derived expectation state of one executed action at `now`. Pure:
 * deterministic for fixed journal state and clock. */
[[nodiscard]] JournalExpectationState
derive_expectation_state(const JournalAction &action,
                         const std::vector<JournalEvent> &events, std::int64_t now);

/* Accounting for one resolution pass. `*_written` counts resolution lines
 * appended by this run; `state_changed` counts appended lines that differ
 * from an earlier recorded state; `skipped_already` counts actions whose
 * terminal state was already recorded. */
struct ResolutionReport {
    std::size_t evaluated{}; /* executed actions carrying an expectation */
    std::size_t pending{};
    std::size_t met_written{};
    std::size_t unmet_written{};
    std::size_t expired_written{};
    std::size_t state_changed{};
    std::size_t skipped_already{};
};

/* Run one bounded, idempotent resolution pass over the journal: derive every
 * executed action's expectation state and append a terminal `met`/`unmet`/
 * `expired` resolution line when it is not already recorded. `now` stamps
 * the resolution lines; `now_rfc3339` is the caller's pre-formatted instant
 * (the same string the other journal entry kinds carry). Returns the count
 * summary. */
[[nodiscard]] ResolutionReport resolve_expectations(const std::filesystem::path &journal_path,
                                                     std::int64_t now,
                                                     std::string_view now_rfc3339);

} // namespace atperson

#endif