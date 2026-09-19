/* Jetstream public backfill client (#60). See jetstream_client.hpp for the
 * contract. This implementation owns the WebSocket transport through
 * Wolfram's `wf_jetstream_*` API; the C23 core never sees a Jetstream type.
 *
 * Ownership notes:
 * - the `wf_jetstream *` handle is created lazily by `connect()` on the first
 *   `fetch_batch` and freed by the destructor or a fatal-error path; a
 *   non-fatal WOULD_BLOCK keeps it alive so Wolfram's internal reconnect can
 *   resume from its own last-delivered cursor;
 * - `wanted_collections` is a `const char *const *`; the strings live in the
 *   client for the whole session and are rebuilt on every (re)connect;
 * - `event.json` is owned by the event struct and freed per event; the
 *   extractor only borrows it during `extract_jetstream_commit`.
 */

#include "jetstream_client.hpp"
#include "atproto/jetstream.hpp"

#include "wolfram/jetstream.h"

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace atperson {

namespace {

/* The Jetstream sequence number is the envelope microsecond timestamp, an
 * opaque int64_t. We store it as a decimal string in the ingestion state and
 * pass it back to Wolfram verbatim; it is never parsed here. */
std::string cursor_from_seq(std::int64_t seq) {
    return seq <= 0 ? std::string() : std::to_string(seq);
}

std::int64_t seq_from_cursor(std::string_view cursor) {
    if (cursor.empty()) {
        return 0;
    }
    try {
        return std::stoll(std::string(cursor));
    } catch (...) {
        return 0;
    }
}

} // namespace

JetstreamClient::JetstreamClient(
    std::string endpoint, std::string self_did,
    std::vector<std::string> collections, std::vector<std::string> dids)
    : endpoint_(std::move(endpoint)), self_did_(std::move(self_did)),
      collections_(std::move(collections)), dids_(std::move(dids)) {
    if (self_did_.empty() || self_did_.rfind("did:", 0u) != 0u) {
        throw std::runtime_error(
            "JetstreamClient: a valid self DID is required for ingestion policy");
    }
    if (collections_.empty()) {
        throw std::runtime_error(
            "JetstreamClient: at least one collection filter is required");
    }
    if (collections_.size() > 100u) {
        throw std::runtime_error(
            "JetstreamClient: more than 100 collection filters are not supported");
    }
    if (dids_.size() > 10000u) {
        throw std::runtime_error(
            "JetstreamClient: more than 10000 DID filters are not supported");
    }
}

JetstreamClient::~JetstreamClient() {
    if (impl_ != nullptr) {
        wf_jetstream_free(static_cast<wf_jetstream *>(impl_));
        impl_ = nullptr;
    }
}

void *JetstreamClient::connect() {
    wf_jetstream_options options{};
    options.endpoint = endpoint_.c_str();

    std::vector<const char *> collection_ptrs;
    collection_ptrs.reserve(collections_.size());
    for (const std::string &collection : collections_) {
        collection_ptrs.push_back(collection.c_str());
    }
    options.wanted_collections = collection_ptrs.data();
    options.wanted_collections_count = collection_ptrs.size();

    std::vector<const char *> did_ptrs;
    did_ptrs.reserve(dids_.size());
    for (const std::string &did : dids_) {
        did_ptrs.push_back(did.c_str());
    }
    options.wanted_dids = did_ptrs.empty() ? nullptr : did_ptrs.data();
    options.wanted_dids_count = did_ptrs.size();
    /* Cursor 0 omits the query parameter entirely; Jetstream starts at the
     * head. A persisted cursor resumes exactly after the last processed frame,
     * which is fine for a public backfill and deduplicated by the ledger. */
    options.cursor = seq_from_cursor(cursor_);
    options.max_message_size_bytes = 0u;
    options.require_hello = 0;
    options.compress = 0;
    options.zstd_dictionary = nullptr;
    options.zstd_dictionary_len = 0u;
    options.reconnect_initial_delay_ms = 250u;
    options.reconnect_max_delay_ms = 30000u;
    options.ping_interval_ms = 0u;

    wf_jetstream *stream = nullptr;
    const wf_status status = wf_jetstream_connect(&options, &stream);
    if (status != WF_OK || stream == nullptr) {
        throw std::runtime_error("JetstreamClient: wf_jetstream_connect failed");
    }
    return stream;
}

std::uint32_t JetstreamClient::reconnect_after_ms() const {
    if (impl_ == nullptr) {
        return 0;
    }
    return wf_jetstream_reconnect_after_ms(static_cast<const wf_jetstream *>(impl_));
}

JetstreamClient::BatchResult JetstreamClient::fetch_batch(
    const JetstreamLimits &limits, std::function<void(const JetstreamEvent &)> on_event) {
    if (impl_ == nullptr) {
        impl_ = connect();
    }
    wf_jetstream *stream = static_cast<wf_jetstream *>(impl_);

    BatchResult result;
    const auto start = std::chrono::steady_clock::now();

    while (true) {
        if (limits.max_events > 0 &&
            result.frames_consumed >= limits.max_events) {
            break;
        }
        if (limits.max_ms > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
            if (elapsed >= limits.max_ms) {
                break;
            }
        }

        wf_jetstream_event event{};
        const wf_status status = wf_jetstream_next(stream, &event);
        if (status == WF_ERR_WOULD_BLOCK) {
            /*
             * Idle socket or reconnect backoff: not a failure. The caller may
             * sleep for reconnect_after_ms() and retry this batch.
             */
            return result;
        }
        if (status == WF_ERR_PARSE) {
            /*
             * Wolfram has already consumed this WebSocket message. Count it
             * against the work budget and skip it; one malformed public frame
             * must not tear down the entire ingestion run.
             *
             * No cursor can safely be derived from an unparseable envelope.
             * The next valid frame advances the stream cursor. If a reconnect
             * occurs first, replaying and skipping this frame again is safer
             * than fabricating a cursor.
             */
            wf_jetstream_event_free(&event);
            ++result.frames_consumed;
            ++result.malformed_frames;
            continue;
        }
        if (status != WF_OK) {
            wf_jetstream_event_free(&event);
            wf_jetstream_free(stream);
            impl_ = nullptr;
            throw std::runtime_error("JetstreamClient: wf_jetstream_next failed");
        }

        ++result.frames_consumed;
        if (event.kind == WF_JETSTREAM_EVENT_COMMIT && event.did != nullptr &&
            event.json != nullptr) {
            SyncObservation observation;
            if (atperson::extract_jetstream_commit(
                    event.json, event.json_len, self_did_, observation)) {
                JetstreamEvent js;
                js.source_uri = observation.source_uri;
                js.author_did = observation.author_did;
                js.created_at = observation.created_at;
                js.text = observation.text;
                js.reply_root = observation.context.reply_root_uri;
                js.reply_parent = observation.context.reply_parent_uri;
                js.quote_uri = observation.context.quote_uri;
                js.seq = event.time_us;
                on_event(js);
            }
        }

        if (event.time_us > 0) {
            cursor_ = cursor_from_seq(event.time_us);
        }

        wf_jetstream_event_free(&event);
    }

    /* Budget exhausted: the connection stays open for the next batch. */
    return result;
}

} // namespace atperson