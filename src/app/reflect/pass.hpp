#ifndef ATPERSON_REFLECT_PASS_HPP
#define ATPERSON_REFLECT_PASS_HPP

// Deterministic reflection pass (#151): the single bounded, non-training unit
// that turns durable state into thought store entries.
//
// The pass reads the action journal (`JournalContents`) and the learned graph
// (`LanguageGraph`) and writes derived thoughts into the thought store. It is
// deliberately narrow:
//   - It never calls observe/learn, never mutates the graph or journal, and
//     never feeds a thought back into the training loop. Thoughts are the
//     entity's own derived record, not new experience.
//   - Every word in a thought is templated from counted, named state. No LLM,
//     no free text, no hidden summarisation.
//   - Determinism: identical inputs + identical clock produce identical
//     write sets. Iteration order is append order; ties break on stable keys.
//   - Bounded: a pass writes at most `max_thoughts` entries. Consolidation is
//     gated by `cadence_seconds`; movement triggers are gated by thresholds
//     and deduplicated by (kind, topic, span_end), so a rerun over the same
//     window never duplicates.
//
// Two kinds of output:
//   movement   a threshold trigger — |valence sum| past `valence_delta_min`,
//              `unfamiliar_authors_min` new low-exposure authors, or a
//              |reply-ratio shift| past `reply_ratio_shift_min`. Evaluated on
//              every pass, independently of cadence.
//   consolidation   the scheduled summary of the trailing window (episodes,
//              authors, valence updates, linked events, resolutions); written
//              only after `cadence_seconds` since the previous consolidation.
//
// Contract: the caller owns the store lock (`.thoughts-lock`); the pass only
// appends. The caller passes wall time explicitly (`now`, Unix epoch seconds)
// so tests stay deterministic.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "journal/store.hpp"
#include "reflect/config.hpp"
#include "thought/store.hpp"

#include "atperson/graph.hpp"

namespace atperson {

/* One written thought plus the terse provenance narrative used for the CLI
 * report. */
struct ReflectionResult {
    Thought thought;
    std::string from;
};

struct ReflectionReport {
    std::size_t thoughts_written{0};
    std::vector<ReflectionResult> written;
    /* Trailing-window totals for the report; always filled in, even on the
     * empty window. */
    std::size_t valence_updates_in_window{0};
    std::size_t valence_tokens_in_window{0};
    std::size_t episodes_in_window{0};
    std::size_t authors_in_window{0};
    std::size_t events_in_window{0};
    std::size_t resolutions_in_window{0};
};

[[nodiscard]] ReflectionReport run_reflection_pass(const std::filesystem::path &thoughts_path,
                                                   const JournalContents &journal,
                                                   const LanguageGraph &graph,
                                                   const ReflectionConfig &config,
                                                   std::uint64_t now);

} // namespace atperson

#endif