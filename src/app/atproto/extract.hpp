#ifndef ATPERSON_ATPROTO_EXTRACT_HPP
#define ATPERSON_ATPROTO_EXTRACT_HPP

/*
 * Pure AT Protocol feed-item translation (issue #24).
 *
 * This is the offline-testable half of the network translation layer: it
 * consumes one `app.bsky.feed.defs#feedViewPost` JSON object (already
 * parsed by cJSON, the same parser Wolfram uses) plus the authenticated
 * account DID, and produces a SyncObservation — text, provenance, policy
 * fields, and stable conversational context (reply root/parent URIs,
 * quote target). No HTTP, no Wolfram, no filesystem: the network client
 * owns transport, this owns semantics.
 *
 * Malformed context is handled gracefully: a reply whose parent record is
 * missing or malformed yields empty identifiers rather than a dropped
 * item, and a quote embed that fails to resolve yields no quote URI.
 */

#include "engine.hpp"

#include <cJSON.h>

namespace atperson {

/* Translate one feed item. Returns false when the item is so malformed it
 * cannot become an observation at all (no post object, no URI) — the
 * caller skips it entirely, which is the honest shape for a feed entry that
 * references nothing. */
[[nodiscard]] bool extract_feed_item(cJSON *item, std::string_view account_did,
                                     SyncObservation &out);

} // namespace atperson

#endif
