#ifndef ATPERSON_ATPROTO_JETSTREAM_REPLAY_HPP
#define ATPERSON_ATPROTO_JETSTREAM_REPLAY_HPP

#include "jetstream_client.hpp"

#include <cstddef>
#include <functional>
#include <string_view>

struct wf_jetstream_replay_event;

namespace atperson {

/* Decode one bounded sealed Jetstream segment through Wolfram's .jss decoder
 * and translate commit rows through the same extractor-facing event shape as
 * the live client. Malformed/non-post rows are skipped; transport and decode
 * failures throw before the caller checkpoints anything. */
void decode_jetstream_replay_segment(
    const void *bytes, std::size_t bytes_len, std::string_view self_did,
    const std::function<void(const JetstreamEvent &)> &on_event);

void translate_jetstream_replay_events(
    const wf_jetstream_replay_event *events, std::size_t event_count,
    std::string_view self_did,
    const std::function<void(const JetstreamEvent &)> &on_event);

} // namespace atperson

#endif
