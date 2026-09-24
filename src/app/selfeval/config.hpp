#ifndef ATPERSON_SELFEVAL_CONFIG_HPP
#define ATPERSON_SELFEVAL_CONFIG_HPP

// Operator-facing configuration of the longitudinal self-evaluation pass
// (#153). The pass is strictly read-only over the journal and graph; it
// writes only its own metric snapshot records and never gates, schedules or
// mutates learned state. Environment values are parsed and clamped here so a
// hostile or mistaken variable cannot produce an unbounded pass.

#include <cstddef>
#include <cstdint>

namespace atperson {

inline constexpr std::int64_t kMaxSelfEvalCadenceSeconds = 366ll * 24ll * 3600ll;
inline constexpr std::size_t kMaxSelfEvalTraceItems = 64u;

struct SelfEvalConfig {
    /* ATPERSON_SELFEVAL=1 turns the daemon hook on. */
    bool enabled{false};
    /* Minimum gap between snapshots (default weekly). */
    std::int64_t cadence_seconds{7 * 24 * 3600};
    /* Bounded new-author names kept in the window trace. */
    std::size_t max_trace_authors{8u};
    /* Bounded top-valence-groups kept in the window trace. */
    std::size_t max_trace_groups{8u};
};

[[nodiscard]] SelfEvalConfig self_eval_config_from_environment();

} // namespace atperson

#endif