/* Jetstream commit -> SyncObservation translation (#60).
 *
 * Jetstream delivers a flattened JSON commit (not the firehose CAR shape):
 * the record is already a parsed JSON object inside the commit envelope, so
 * translation is a pure JSON walk over `record` plus the commit's
 * collection/rkey/did/time. This module owns the semantics; the network
 * client owns transport and owns the cJSON tree.
 *
 * The contract mirrors `atproto/extract.hpp`: it consumes one raw Jetstream
 * JSON message (the same bytes `wf_jetstream_event_parse` would consume) and
 * produces a SyncObservation. No HTTP, no Wolfram, no filesystem: this is the
 * offline-testable half, and the network client is responsible for calling
 * `wf_jetstream_event_parse` first and passing the resulting JSON here only
 * when the event kind is a commit.
 *
 * Malformed context is handled gracefully: a reply whose parent record is
 * missing or malformed yields empty identifiers rather than a dropped item,
 * and a quote embed that fails to resolve yields no quote URI.
 */

#ifndef ATPERSON_ATPROTO_JETSTREAM_HPP
#define ATPERSON_ATPROTO_JETSTREAM_HPP

#include "engine.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace atperson {

/* Translate one raw Jetstream JSON commit message into a SyncObservation.
 * Returns false when the message is not a create/update of an
 * app.bsky.feed.post record (deletes, other collections, malformed envelopes
 * and identity/account frames all return false and are skipped by the
 * caller). The feed is unauthenticated, so the ingestion-state source DID is
 * empty on this path and the CLI/daemon owns that identity, never the
 * extractor. */
[[nodiscard]] bool extract_jetstream_commit(
    const char *json, size_t json_len, std::string_view account_did,
    SyncObservation &out);

} // namespace atperson

#endif