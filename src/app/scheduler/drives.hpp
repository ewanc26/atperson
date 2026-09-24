#ifndef ATPERSON_SCHEDULER_DRIVES_HPP
#define ATPERSON_SCHEDULER_DRIVES_HPP

// Experience-derived drives (#148): bounded, deterministic, inspectable
// initiation signals computed purely from existing learned evidence.
//
// Two drives modulate which recent observations the scheduler decides on
// first:
//   curiosity  — novelty * adjacency, per topic (token familiarity) and per
//                author (encounter count). A fresh entity reads zero; a
//                fully-known subject reads zero; only a partially-known
//                subject produces a non-zero drive.
//   reciprocity — the entity acted, and a public record referenced that
//                action. The strongest signal is the candidate observation
//                being exactly the referencing record (source_id ==
//                event_uri); a weaker one is the candidate's author having
//                referenced the entity's action within a bounded window.
//
// Drives are read-only evidence over the graph and the action journal. They
// never mutate learned state and never weaken the scheduler's decision or
// gate bounds; they only reorder candidate contexts. Every signal is clamped
// to [0, 1]; an empty graph and empty journal yield all zeros (a fresh entity
// initiates nothing on its own).
//
// Ownership: no allocations beyond owned strings/vectors. No network, no
// clock — `now` is injected. Failure semantics: journal timestamps that fail
// to parse are treated as outside the reciprocity window (best-effort
// evidence, never thrown). `select_candidates` mirrors the scheduler's
// context selection so inspection and scheduling never diverge.

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace atperson {

struct JournalContents; /* journal/store.hpp */

namespace drives {

/* The evidence the scheduler already holds for one candidate context. */
struct ContextCandidate {
    std::uint64_t ledger_id{};
    std::string payload;
    std::string source_id;
    std::string author_did;
    std::uint64_t observed_at{}; /* unix seconds */
};

inline constexpr const char *kReciprocityNone = "none";
inline constexpr const char *kReciprocityEvent = "event";
inline constexpr const char *kReciprocityAuthor = "author";

/* Clamped [0, 1] derived signals for one candidate. */
struct Signals {
    float curiosity{};
    float reciprocity{};
    /* How reciprocity was earned: "none", "event" (this record is the
     * referencing event) or "author" (this author recently referenced the
     * entity's action). */
    const char *reciprocity_source{kReciprocityNone};
};

/* Bounded window for the "this author recently referenced the entity's
 * action" reciprocity signal, in seconds. */
inline constexpr std::int64_t kReciprocityWindowSeconds = 7 * 24 * 3600;

/* The most recent committed, payload-bearing ledger entries, newest first,
 * bounded to max_contexts: exactly the scheduler's context selection. */
[[nodiscard]] std::vector<ContextCandidate> select_candidates(const Ledger &ledger,
                                                              std::size_t max_contexts);

/* Compute both drive signals for every candidate, purely from the graph and
 * the journal. Deterministic for fixed inputs. */
[[nodiscard]] std::vector<Signals> compute_drive_signals(const LanguageGraph &graph,
                                                         const std::vector<ContextCandidate> &candidates,
                                                         const JournalContents &journal,
                                                         std::int64_t now);

/* Index permutation over `signals`: reciprocity desc, curiosity desc, then
 * stable (original) order. Returns indices into the input sequence. */
[[nodiscard]] std::vector<std::size_t> order_candidates(const std::vector<Signals> &signals);

} // namespace drives
} // namespace atperson

#endif