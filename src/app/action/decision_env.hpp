#ifndef ATPERSON_ACTION_DECISION_ENV_HPP
#define ATPERSON_ACTION_DECISION_ENV_HPP

// Operator opt-in for the guarded decision layer's valence veto.
//
// ATPERSON_DECISION_MIN_VALENCE unset or empty leaves the guard off (decisions
// are exactly what they were). A value in [-1, 0] turns it on with that
// threshold: a candidate whose explicit, experience-derived valence is below it
// is rejected. Anything else is a configuration error and throws, so a typo can
// never silently disable (or invert) the guard. Header-only so every target
// that builds a decision config shares one parser.

#include "atperson/action.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace atperson {

inline void apply_decision_env(atp_action_decision_config &config) {
    const char *raw = std::getenv("ATPERSON_DECISION_MIN_VALENCE");
    if (raw == nullptr || raw[0] == '\0') {
        return;
    }
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(raw, &end);
    if (end == raw || *end != '\0' || errno != 0 || !std::isfinite(value) || value < -1.0 ||
        value > 0.0) {
        throw std::runtime_error(
            "ATPERSON_DECISION_MIN_VALENCE must be a number between -1 and 0");
    }
    config.guards.valence_guard = true;
    config.guards.min_valence = static_cast<float>(value);
}

} // namespace atperson

#endif
