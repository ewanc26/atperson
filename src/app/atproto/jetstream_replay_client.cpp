#include "jetstream_replay_client.hpp"

#include "jetstream_replay.hpp"

#include "wolfram/agent.h"
#include "wolfram/jetstream_replay.h"
#include "wolfram/xrpc.h"

#include <stdexcept>

namespace atperson {

JetstreamReplayClient::WindowResult JetstreamReplayClient::fetch_window(
    std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
    std::string_view self_did, const std::vector<std::string> &collections,
    const std::vector<std::string> &dids,
    const std::function<void(const JetstreamEvent &)> &on_event) {
    if (!on_event || self_did.empty() || collections.empty()) {
        throw std::invalid_argument("invalid Jetstream replay window arguments");
    }
    const char *kinds[] = {"commit"};
    std::vector<const char *> collection_values;
    collection_values.reserve(collections.size());
    for (const auto &collection : collections) collection_values.push_back(collection.c_str());
    std::vector<const char *> did_values;
    did_values.reserve(dids.size());
    for (const auto &did : dids) did_values.push_back(did.c_str());
    wf_jetstream_replay_filter filter{};
    filter.kinds = kinds;
    filter.kinds_count = 1u;
    filter.collections = collection_values.data();
    filter.collections_count = collection_values.size();
    filter.dids = did_values.data();
    filter.dids_count = did_values.size();
    filter.after_seq = after_seq;
    if (before_seq) {
        filter.before_seq = *before_seq;
        filter.has_before_seq = 1;
    }
    wf_xrpc_client *const client = wf_agent_get_xrpc_client(&agent_);
    if (client == nullptr) {
        throw std::runtime_error("Jetstream replay requires an authenticated agent client");
    }

    WindowResult result;
    for (;;) {
        wf_jetstream_replay_plan_page page{};
        if (wf_jetstream_replay_plan(client, &filter, &page) != WF_OK) {
            wf_jetstream_replay_plan_page_free(&page);
            throw std::runtime_error("Jetstream replay planSnapshot failed");
        }
        if (!before_seq) {
            before_seq = page.sealed_tip_seq;
        }
        result.sealed_tip_seq = *before_seq;
        try {
            for (std::size_t i = 0u; i < page.segments_count; ++i) {
                const auto &segment = page.segments[i];
                if (segment.name == nullptr) {
                    throw std::runtime_error("Jetstream replay plan contained unnamed segment");
                }
                if (segment.mode == WF_JETSTREAM_REPLAY_SEGMENT_WHOLE) {
                    wf_response response{};
                    if (wf_jetstream_replay_get_segment(client, segment.name, &response) != WF_OK) {
                        wf_response_free(&response);
                        throw std::runtime_error("Jetstream replay getSegment failed");
                    }
                    decode_jetstream_replay_segment(response.body, response.body_len, self_did,
                                                    on_event);
                    wf_response_free(&response);
                } else if (segment.mode == WF_JETSTREAM_REPLAY_SEGMENT_BLOCKS) {
                    for (std::size_t b = 0u; b < segment.blocks_count; ++b) {
                        for (std::uint64_t index = segment.blocks[b].first;; ++index) {
                            wf_response response{};
                            if (wf_jetstream_replay_get_block(client, segment.name, index,
                                                              &response) != WF_OK) {
                                wf_response_free(&response);
                                throw std::runtime_error("Jetstream replay getBlock failed");
                            }
                            wf_jetstream_replay_event *events = nullptr;
                            std::size_t event_count = 0u;
                            const wf_status status = wf_jetstream_replay_block_decode_zstd(
                                response.body, response.body_len, &events, &event_count);
                            wf_response_free(&response);
                            if (status != WF_OK) {
                                wf_jetstream_replay_events_free(events, event_count);
                                throw std::runtime_error("Jetstream replay block decode failed");
                            }
                            try {
                                translate_jetstream_replay_events(events, event_count, self_did,
                                                                   on_event);
                            } catch (...) {
                                wf_jetstream_replay_events_free(events, event_count);
                                throw;
                            }
                            wf_jetstream_replay_events_free(events, event_count);
                            if (index == segment.blocks[b].last) break;
                        }
                    }
                }
            }
        } catch (...) {
            wf_jetstream_replay_plan_page_free(&page);
            throw;
        }
        const std::uint64_t planned = page.planned_through_seq;
        const std::uint64_t sealed = page.sealed_tip_seq;
        wf_jetstream_replay_plan_page_free(&page);
        result.planned_through_seq = planned;
        if (planned >= sealed || planned >= *before_seq) return result;
        if (planned <= filter.after_seq) {
            throw std::runtime_error("Jetstream replay plan did not advance");
        }
        filter.after_seq = planned;
        filter.before_seq = *before_seq;
        filter.has_before_seq = 1;
    }
}

} // namespace atperson
