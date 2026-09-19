#ifndef ATPERSON_ATPROTO_JETSTREAM_FILTER_HPP
#define ATPERSON_ATPROTO_JETSTREAM_FILTER_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace atperson {

inline constexpr std::size_t kJetstreamCollectionFilterLimit = 100u;

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

} // namespace atperson

#endif
