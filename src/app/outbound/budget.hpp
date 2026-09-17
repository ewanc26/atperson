#ifndef ATPERSON_OUTBOUND_BUDGET_HPP
#define ATPERSON_OUTBOUND_BUDGET_HPP

// Outbound rate-budget state (#23): the rolling record of recently admitted
// actions that makes rate limits survive a restart.
//
// This is runtime metadata, not learned C23 state and not part of the model
// snapshot. It is persisted atomically to `ATPERSON_OUTBOUND_BUDGET`
// (default `<data>/outbound-budget.json`) so a crash or restart cannot
// accidentally reset the window and permit a burst: the recorded timestamps
// are reloaded and the same limits apply.
//
// Ownership: `OutboundBudgetState` is plain value state. `record` and
// `prune` mutate it in place; the caller owns serialisation. This module has
// no locking: the state is single-writer by contract and must be mutated
// through one owner thread or an explicit serialisation point.
//
// Fail-closed posture: a missing file yields empty (nothing recorded yet)
// state; a present but malformed file throws, so a caller refuses to write
// rather than guessing. Timestamps are monotonic per kind: a backwards clock
// is clamped to the last recorded action so windows cannot shrink.

#include "actions.hpp"
#include "config.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace atperson {

/* Per-kind cap on retained records; bounds the state file and the cost of
 * every evaluation. */
inline constexpr std::size_t kMaxOutboundRecordsPerKind = 256u;

struct OutboundDuplicateRecord {
    std::string key;
    std::int64_t at{0};
};

struct OutboundKindUsage {
    /* Unix seconds of the most recent admitted action; nullopt when none.
     * A separate flag, not 0, because 0 is a valid timestamp. */
    std::optional<std::int64_t> last_action_at;
    /* Ascending (non-decreasing) unix seconds of recent admitted actions. */
    std::vector<std::int64_t> recent_actions;
    /* Recently admitted action identities, oldest first. */
    std::vector<OutboundDuplicateRecord> recent_duplicates;
};

struct OutboundBudgetState {
    std::uint32_t version{1u};
    std::array<OutboundKindUsage, kOutboundActionKindCount> usage{};
    std::optional<std::int64_t> saved_at;
};

/* Thrown for malformed budget documents and impossible records. */
class OutboundBudgetError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Missing file -> empty state. Malformed file -> OutboundBudgetError. */
[[nodiscard]] OutboundBudgetState load_outbound_budget_state(const std::filesystem::path &path);

[[nodiscard]] std::string serialise_outbound_budget_state(const OutboundBudgetState &state);

/* Atomic write-and-rename; throws std::runtime_error on I/O failure. */
void save_outbound_budget_state(const OutboundBudgetState &state,
                                const std::filesystem::path &path);

/* Drop records that can no longer affect `budget`: actions older than its
 * window/spacing horizon and duplicates older than its cooldown. Also
 * enforces the per-kind cap. Deterministic for fixed inputs. */
void prune_outbound_budget_state(OutboundBudgetState &state, const OutboundPolicy &policy,
                                 std::int64_t now);

/* Record one admitted proposal against `budget`. Clamps a backwards clock to
 * the last recorded action, then prunes. Mutates only this state. */
void record_outbound_action(OutboundBudgetState &state, const OutboundActionProposal &proposal,
                            const ActionBudget &budget, std::int64_t now);

/* Number of recorded actions of `kind` within the trailing window at `now`. */
[[nodiscard]] std::uint32_t count_outbound_actions_in_window(const OutboundBudgetState &state,
                                                             OutboundActionKind kind,
                                                             std::int64_t window_seconds,
                                                             std::int64_t now);

/* Unix seconds of the last admitted action of `kind`, or nullopt when none. */
[[nodiscard]] std::optional<std::int64_t> last_outbound_action_at(const OutboundBudgetState &state,
                                                                  OutboundActionKind kind);

/* Unix seconds of the oldest recorded action of `kind` still inside the
 * trailing window at `now`, or nullopt when the window is empty. */
[[nodiscard]] std::optional<std::int64_t>
earliest_outbound_action_in_window(const OutboundBudgetState &state, OutboundActionKind kind,
                                   std::int64_t window_seconds, std::int64_t now);

/* Unix seconds when this exact identity was last admitted, or nullopt when
 * none. */
[[nodiscard]] std::optional<std::int64_t>
duplicate_outbound_recorded_at(const OutboundBudgetState &state,
                               const OutboundActionProposal &proposal);

} // namespace atperson

#endif
