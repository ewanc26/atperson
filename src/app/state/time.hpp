#ifndef ATPERSON_STATE_TIME_HPP
#define ATPERSON_STATE_TIME_HPP

// RFC 3339 timestamp parsing shared by the runtime (#56).
//
// One implementation, one owner: src/app/state/time.cpp. Previously a
// static helper inside src/app/sync/engine.cpp; extracted when the
// outcome-to-valence mapping (#56) needed the same parser for its
// within-seconds windows. Pure function, no clock, no state.

#include <cstdint>
#include <optional>
#include <string_view>

namespace atperson {

/*
 * Parse an RFC 3339 timestamp ("YYYY-MM-DDTHH:MM:SS[.frac][Z|±HH:MM]") to
 * Unix epoch seconds; nullopt for anything that is not a full, valid
 * instant. Callers treat unknown times as 0 (unknown) rather than
 * inventing an instant.
 */
[[nodiscard]] std::optional<std::uint64_t> parse_rfc3339_epoch(std::string_view value);

} // namespace atperson

#endif
