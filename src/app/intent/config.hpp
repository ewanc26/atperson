#ifndef ATPERSON_INTENT_CONFIG_HPP
#define ATPERSON_INTENT_CONFIG_HPP

// Pending social intent configuration (#150): the operator-facing bounds for
// the conversation-continuation feature.
//
// This is runtime policy, never learned state. It is read from the
// environment (the same surface the scheduler and drives use) and is off
// unless explicitly enabled:
//   ATPERSON_INTENTS=1                  master switch (default off)
//   ATPERSON_INTENT_MAX_ACTIVE          maximum concurrent open intents
//   ATPERSON_INTENT_MAX_CONTINUATIONS   replies per conversation before close
//   ATPERSON_INTENT_WINDOW_DAYS         reply window per intent, in days
//
// Bounds are enforced (clamped) so a malformed or hostile environment can
// never widen a cap past the documented ceiling. Defaults keep the feature
// inert: it must be explicitly enabled and its caps are small.
//
// Ownership: `IntentConfig` is plain value state; callers own it. No hidden
// global state, no I/O beyond getenv.

#include <cstddef>
#include <cstdint>

namespace atperson {

struct IntentConfig {
    bool enabled{false};
    /* Maximum concurrent open intents; opening beyond this refuses (the
     * oldest intent is never silently dropped — it expires explicitly). */
    std::size_t max_active{3u};
    /* Maximum replies the entity makes in one conversation before the
     * intent closes: the "must not orbit one conversation forever" bound. */
    std::uint32_t max_continuations{3u};
    /* Default reply window, in seconds, from the intent's opening entry. */
    std::int64_t window_seconds{2 * 24 * 3600};
};

inline constexpr std::size_t kMaxActiveIntents = 256u;
inline constexpr std::uint32_t kMaxIntentContinuations = 16u;
inline constexpr std::int64_t kMaxIntentWindowSeconds = 366ll * 24ll * 3600ll;

/* Current configuration from the environment; sane defaults when a knob is
 * missing or unparsable, clamped to the documented ceilings when present. */
[[nodiscard]] IntentConfig intent_config_from_environment();

} // namespace atperson

#endif