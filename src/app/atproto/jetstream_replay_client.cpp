#include "jetstream_replay_client.hpp"

#include "jetstream_replay.hpp"

#include "wolfram/jetstream_replay.h"
#include "wolfram/xrpc.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace atperson {

namespace {

/* Default public Jetstream archive host. The archive API (planSnapshot,
 * getSegment, getBlock) is served here, not on the PDS. */
inline constexpr const char *kDefaultJetstreamArchiveHost =
    "https://jetstream.us-west.bsky.network";

} // namespace

namespace {

/* Translates a rejected archive request into a distinct, greppable error so a
 * dead/expired archive token fails the backfill fast instead of retrying as
 * if it were a transient network failure. */
[[noreturn]] void throw_archive_transport_error(const char *what) {
    throw std::runtime_error(std::string("Jetstream archive ") + what +
                             " failed: archive rejected credentials (WF_ERR_AUTH) — "
                             "check ATPERSON_JETSTREAM_ARCHIVE_TOKEN");
}

} // namespace

void JetstreamReplayClient::XrpcClientDeleter::operator()(
    wf_xrpc_client *client) const noexcept {
    wf_xrpc_client_free(client);
}

JetstreamReplayClient::JetstreamReplayClient(std::string archive_host,
                                             std::string archive_token)
    : archive_token_(std::move(archive_token)) {
    if (archive_token_.empty()) {
        throw std::runtime_error(
            "Jetstream replay requires ATPERSON_JETSTREAM_ARCHIVE_TOKEN");
    }
    if (archive_host.empty()) {
        archive_host = kDefaultJetstreamArchiveHost;
    }
    wf_xrpc_client *const client = wf_xrpc_client_new(archive_host.c_str());
    if (client == nullptr) {
        throw std::runtime_error("Jetstream replay failed to create transport");
    }
    client_.reset(client);
    /* Archive endpoints use a raw archive API token, not the PDS session JWT. */
    wf_xrpc_client_set_auth(client_.get(), archive_token_.c_str());
}

JetstreamReplayClient::~JetstreamReplayClient() = default;

std::optional<std::uint64_t> JetstreamReplayClient::probe_sealed_tip() {
    /* The archive clamps sealedTipSeq to the request's beforeSeq, so the
     * probe sends no upper bound at all and starts beyond any reachable
     * sequence (the JSON-safe exact integer cap, 2^53-1). Nothing matches,
     * but the response carries the unclamped global tip. */
    wf_jetstream_replay_filter filter{};
    filter.after_seq = 9007199254740991u;
    filter.has_before_seq = 0;
    wf_jetstream_replay_plan_page page{};
    const wf_status probe_status =
        wf_jetstream_replay_plan(client_.get(), &filter, &page);
    if (probe_status != WF_OK) {
        wf_jetstream_replay_plan_page_free(&page);
        if (probe_status == WF_ERR_AUTH) throw_archive_transport_error("tip probe");
        throw std::runtime_error("Jetstream replay tip probe failed");
    }
    const std::uint64_t tip = page.sealed_tip_seq;
    wf_jetstream_replay_plan_page_free(&page);
    if (tip == 0u) {
        return std::nullopt;
    }
    return tip;
}

JetstreamReplayWindow JetstreamReplayClient::fetch_window(
    std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
    std::string_view self_did, const std::vector<std::string> &collections,
    const std::vector<std::string> &dids,
    const std::function<void(const JetstreamEvent &)> &on_event) {
    if (!on_event || self_did.empty() || collections.empty()) {
        throw std::invalid_argument("invalid Jetstream replay window arguments");
    }
    const bool caller_supplied_before = before_seq.has_value();
    /* The archive clamps sealedTipSeq to the request's beforeSeq, so a window
     * cannot learn the true tip from its own plan response. Probe it once:
     * the tip drives both the auto-window clamp and the exhaustion signal. */
    const std::optional<std::uint64_t> true_tip = probe_sealed_tip();
    if (!caller_supplied_before) {
        if (true_tip && *true_tip < before_seq) {
            before_seq = *true_tip;
        }
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
    const auto bounded_before = bounded_jetstream_replay_before(after_seq, before_seq);
    if (!bounded_before) {
        throw std::invalid_argument("Jetstream replay window exceeds the hard sequence cap");
    }
    before_seq = bounded_before;
    filter.after_seq = after_seq;
    filter.before_seq = *before_seq;
    filter.has_before_seq = 1;
    wf_xrpc_client *const client = client_.get();
    JetstreamReplayWindow result;
    for (;;) {
        wf_jetstream_replay_plan_page page{};
        const wf_status plan_status = wf_jetstream_replay_plan(client_.get(), &filter, &page);
        if (plan_status != WF_OK) {
            wf_jetstream_replay_plan_page_free(&page);
            if (plan_status == WF_ERR_AUTH) throw_archive_transport_error("planSnapshot");
            throw std::runtime_error("Jetstream replay planSnapshot failed");
        }
        result.sealed_tip_seq = true_tip.value_or(*before_seq);
        try {
            for (std::size_t i = 0u; i < page.segments_count; ++i) {
                const auto &segment = page.segments[i];
                if (segment.name == nullptr) {
                    throw std::runtime_error("Jetstream replay plan contained unnamed segment");
                }
                if (segment.mode == WF_JETSTREAM_REPLAY_SEGMENT_WHOLE) {
                    wf_response response{};
                    const wf_status segment_status =
                        wf_jetstream_replay_get_segment(client, segment.name, &response);
                    if (segment_status != WF_OK) {
                        wf_response_free(&response);
                        if (segment_status == WF_ERR_AUTH) {
                            throw_archive_transport_error("getSegment");
                        }
                        throw std::runtime_error("Jetstream replay getSegment failed");
                    }
                    decode_jetstream_replay_segment(response.body, response.body_len, self_did,
                                                    on_event);
                    wf_response_free(&response);
                } else if (segment.mode == WF_JETSTREAM_REPLAY_SEGMENT_BLOCKS) {
                    for (std::size_t b = 0u; b < segment.blocks_count; ++b) {
                        for (std::uint64_t index = segment.blocks[b].first;; ++index) {
                            wf_response response{};
                            const wf_status block_status =
                                wf_jetstream_replay_get_block(client, segment.name, index,
                                                              &response);
                            if (block_status != WF_OK) {
                                wf_response_free(&response);
                                if (block_status == WF_ERR_AUTH) {
                                    throw_archive_transport_error("getBlock");
                                }
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
