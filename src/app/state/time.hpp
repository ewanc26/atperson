#ifndef ATPERSON_STATE_TIME_HPP
#define ATPERSON_STATE_TIME_HPP

// RFC 3339 timestamp parsing and formatting shared by the runtime (#56).
//
// One implementation, one owner: src/app/state/time.cpp. Previously a
// static helper inside src/app/sync/engine.cpp; extracted when the
// outcome-to-valence mapping (#56) needed the same parser for its
// within-seconds windows, and later given the formatter the scheduler and
// intent paths need. Pure functions, no clock, no state.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace atperson {

/*
 * Parse an RFC 3339 timestamp ("YYYY-MM-DDTHH:MM:SS[.frac][Z|±HH:MM]") to
 * Unix epoch seconds; nullopt for anything that is not a full, valid
 * instant. Callers treat unknown times as 0 (unknown) rather than
 * inventing an instant.
 */
[[nodiscard]] std::optional<std::uint64_t> parse_rfc3339_epoch(std::string_view value);

/*
 * Format Unix epoch seconds as an RFC 3339 UTC timestamp
 * ("YYYY-MM-DDTHH:MM:SSZ"). One formatting owner; callers wanting the same
 * instant string used by the journal/audit lines share this rather than
 * duplicating the gmtime_r buffer dance.
 */
[[nodiscard]] std::string rfc3339_from_unix(std::int64_t unix_seconds);

} // namespace atperson

#endif
