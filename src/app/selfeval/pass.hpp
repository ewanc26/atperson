#ifndef ATPERSON_SELFEVAL_PASS_HPP
#define ATPERSON_SELFEVAL_PASS_HPP

// Longitudinal self-evaluation pass (#153): the single bounded, read-only unit
// that turns durable state into one schema-versioned metric snapshot.
//
// The pass reads the action journal (`JournalContents`) and the learned graph
// (`LanguageGraph`) and appends one snapshot record to the metric store. It is
// deliberately narrow:
//   - It never calls observe/learn/recall, never mutates the graph or journal,
//     and never feeds a snapshot back into the training loop. Operator-mediated
//     only: it never gates behaviour, budgets or schedules.
//   - Metrics are derived (cumulative actions executed vs. admitted, terminal
//     intent success per #150, cumulative reply/quote ratio, cumulative valence
//     drift, familiarity growth) plus a bounded per-period trace so every
//     number carries provenance.
//   - Cadence: a snapshot is written only after `cadence_seconds` since the
//     previous snapshot's record time (or when none exists). A rerun over the
//     same inputs and clock writes nothing new, so the write set of two passes
//     over identical state is identical.
//
// Contract: the caller owns the store lock; the pass only appends. The caller
// passes wall time explicitly (`now`, Unix epoch seconds) so tests stay
// deterministic.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "journal/store.hpp"
#include "selfeval/config.hpp"
#include "selfeval/store.hpp"

#include "atperson/graph.hpp"

namespace atperson {

struct SelfEvalReport {
    /* Whether this pass appended a snapshot. */
    bool due{false};
    /* Why: "no prior snapshot", "cadence elapsed" or "inside cadence". */
    std::string reason;
    /* The snapshot appended when `due`; empty otherwise. */
    std::optional<MetricSnapshot> snapshot;
    /* Snapshots present before this pass. */
    std::size_t prior_snapshots{0u};
};

[[nodiscard]] SelfEvalReport run_self_eval_pass(const std::filesystem::path &metrics_dir,
                                                const JournalContents &journal,
                                                const LanguageGraph &graph,
                                                const SelfEvalConfig &config,
                                                std::uint64_t now);

} // namespace atperson

#endif