/* Jetstream zstd dictionary fetch (#60 full-speed live ingestion).
 *
 * The official Jetstream dictionary is served by the public
 * `network.bsky.jetstream.getZstdDictionary` XRPC query. This module fetches
 * it through Wolfram's HTTP transport — no credentials, no session — and
 * returns the raw dictionary bytes the caller passes to
 * `wf_jetstream_options.zstd_dictionary` (connect copies it).
 *
 * Failure modes: throws std::runtime_error on transport failure, non-200
 * status, or an empty body. A caller that cannot fetch the dictionary must
 * fall back to uncompressed JSON frames rather than failing the run.
 */

#ifndef ATPERSON_ATPROTO_JETSTREAM_DICTIONARY_HPP
#define ATPERSON_ATPROTO_JETSTREAM_DICTIONARY_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace atperson {

/* Absolute https base of the Jetstream service that serves the dictionary
 * query (for example https://jetstream.us-east.bsky.network). */
[[nodiscard]] std::string
fetch_jetstream_zstd_dictionary(std::string_view service_base_url);

} // namespace atperson

#endif
