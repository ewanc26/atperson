#ifndef ATPERSON_REFLECT_CONFIG_HPP
#define ATPERSON_REFLECT_CONFIG_HPP

// Deterministic reflection configuration (#151): the operator-facing bounds
// for the bounded, non-training reflection pass.
//
// This is runtime policy, never learned state. It is read from the
// environment (the same surface the scheduler, drives and intents use) and
// is off unless explicitly enabled:
//   ATPERSON_REFLECTION=1                      master switch (default off)
//   ATPERSON_REFLECTION_CADENCE_SECONDS        minimum gap before a new
//                                              consolidation thought
//   ATPERSON_REFLECTION_MAX_THOUGHTS           maximum thoughts one pass may
//                                              write
//   ATPERSON_REFLECTION_WINDOW_SECONDS         trailing analysis window size
//   ATPERSON_REFLECTION_VALENCE_DELTA_MIN      |unit-agnostic valence sum|
//                                              floor for a valence trigger
//   ATPERSON_REFLECTION_UNFAMILIAR_MIN         minimum distinct unfamiliar
//                                              authors in window for an
//                                              author trigger
//   ATPERSON_REFLECTION_REPLY_SHIFT_MIN        |reply-ratio movement| floor
//                                              for a reply trigger
//
// Bounds are enforced (clamped) so a malformed or hostile environment can
// never widen a cap past the documented ceiling, and every knob defaults to
// inert, bounded behaviour: the pass never writes more than the ceiling
// allows and never consolidates more often than the cadence permits.
//
// Ownership: `ReflectionConfig` is plain value state; callers own it. No
// hidden global state, no I/O beyond getenv.

#include <cstddef>
#include <cstdint>

namespace atperson {

struct ReflectionConfig {
    bool enabled{false};
    /* Minimum seconds between consolidation thoughts; a smaller cadence
     * means a more chatty, more frequently scheduled pass. */
    std::int64_t cadence_seconds{24 * 3600};
    /* Maximum thoughts one pass may write (consolidation + triggers). */
    std::size_t max_thoughts{8u};
    /* Trailing analysis window, in seconds. */
    std::int64_t window_seconds{7 * 24 * 3600};
    /* |sum of movement signals in window| must reach this floor for a
     * valence trigger. */
    float valence_delta_min{0.25f};
    /* Distinct unfamiliar authors (exposure below two encounters) in the
     * window must reach this count for an author trigger. */
    std::size_t unfamiliar_authors_min{2u};
    /* |current reply ratio - previous reply ratio| must reach this shift for
     * a reply trigger. */
    float reply_ratio_shift_min{0.25f};
};

inline constexpr std::int64_t kMaxReflectionCadenceSeconds = 366ll * 24ll * 3600ll;
inline constexpr std::size_t kMaxReflectionThoughts = 64u;
inline constexpr std::int64_t kMaxReflectionWindowSeconds = 366ll * 24ll * 3600ll;
inline constexpr float kMaxReflectionDelta = 1.0f;

/* Current configuration from the environment; sane defaults when a knob is
 * missing or unparsable, clamped to the documented ceilings when present. */
[[nodiscard]] ReflectionConfig reflection_config_from_environment();

} // namespace atperson

#endif