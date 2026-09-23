#ifndef ATPERSON_ATPROTO_JETSTREAM_FILTER_HPP
#define ATPERSON_ATPROTO_JETSTREAM_FILTER_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace atperson {

inline constexpr std::size_t kJetstreamCollectionFilterLimit = 100u;
inline constexpr std::size_t kJetstreamDidFilterLimit = 10000u;
inline constexpr std::size_t kJetstreamKindFilterLimit = 4u;

/*
 * The default learning slice remains public Bluesky posts. Transport filters
 * are runtime/operator configuration only; they never become learned state.
 */
[[nodiscard]] std::vector<std::string> default_jetstream_collections();

/*
 * Load one wantedCollections value per line. Blank lines and lines beginning
 * with '#' after trimming are ignored. Duplicate entries are removed while
 * preserving first occurrence order. At least one and at most 100 unique
 * entries are required, matching Wolfram/Jetstream's subscription limit.
 *
 * Tokens may include Jetstream wildcard syntax (for example app.bsky.graph.*)
 * but may not contain ASCII whitespace. Throws std::runtime_error for I/O or
 * validation failure.
 */
[[nodiscard]] std::vector<std::string>
load_jetstream_collections(const std::filesystem::path &path);

/*
 * Load one wantedDids value per line with the same comment/blank/dedup rules.
 * Every value must begin with "did:" and at most 10,000 unique DIDs are
 * accepted, matching Wolfram/Jetstream's subscription bound.
 */
[[nodiscard]] std::vector<std::string>
load_jetstream_dids(const std::filesystem::path &path);

/*
 * Load one v2 event kind per line with the same comment/blank/dedup rules.
 * Only the Jetstream v2 kinds are accepted: commit, identity, account, sync.
 * At least one and at most 4 unique entries are required, matching
 * Wolfram/Jetstream's subscription bound. An empty kind predicate retains
 * commits plus #sync, #identity and #account events for the protocol-evidence
 * ledger; kinds are a transport filter, never learned state.
 */
[[nodiscard]] std::vector<std::string>
load_jetstream_kinds(const std::filesystem::path &path);

/*
 * Load one v2 event kind per line with the same comment/blank/dedup rules.
 * Only the Jetstream v2 kinds are accepted: commit, identity, account, sync.
 * At least one and at most 4 unique entries are required, matching
 * Wolfram/Jetstream's subscription bound. An empty kind predicate retains
 * commits plus #sync, #identity and #account events for the protocol-evidence
 * ledger; kinds are a transport filter, never learned state.
 */
[[nodiscard]] std::vector<std::string>
load_jetstream_kinds(const std::filesystem::path &path);

} // namespace atperson

#endif
