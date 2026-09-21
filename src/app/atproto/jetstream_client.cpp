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

/* v1 uses the envelope timestamp while v2 uses the envelope sequence. Both
 * are decimal cursors, but they are not interchangeable across reconnects. */
std::string cursor_from_seq(std::int64_t seq) {
    return seq <= 0 ? std::string() : std::to_string(seq);
}

std::int64_t seq_from_cursor(std::string_view cursor) {
    if (cursor.empty()) {
        return 0;
    }
    std::size_t consumed = 0u;
    try {
        const std::int64_t value = std::stoll(std::string(cursor), &consumed, 10);
        if (consumed != cursor.size() || value <= 0) {
            throw std::runtime_error("invalid Jetstream cursor");
        }
        return value;
    } catch (const std::exception &) {
        throw std::runtime_error(
            "JetstreamClient: persisted cursor is not a positive decimal timestamp");
    }
}

} // namespace

JetstreamClient::JetstreamClient(
    std::string endpoint, std::string self_did,
    std::vector<std::string> collections, std::vector<std::string> dids,
    std::string initial_cursor)
    : endpoint_(std::move(endpoint)), self_did_(std::move(self_did)),
      collections_(std::move(collections)), dids_(std::move(dids)),
      protocol_v2_(endpoint_.find("/xrpc/network.bsky.jetstream.subscribeEvents") !=
                   std::string::npos),
      cursor_(std::move(initial_cursor)) {
    if (!self_did_.empty() && self_did_.rfind("did:", 0u) != 0u) {
        throw std::runtime_error(
            "JetstreamClient: self DID must be empty or begin with 'did:'");
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
    if (!cursor_.empty()) {
        static_cast<void>(seq_from_cursor(cursor_));
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
    options.protocol_version = protocol_v2_ ? 2 : 1;

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
    static const char *const v2_kinds[] = {"commit"};
    options.kinds = protocol_v2_ ? v2_kinds : nullptr;
    options.kinds_count = protocol_v2_ ? 1u : 0u;
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
        throw std::runtime_error(
            "JetstreamClient: wf_jetstream_connect failed for " + endpoint_ +
            " (native status " + std::to_string(static_cast<int>(status)) + ")");
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
            wf_jetstream_event_typed typed{};
            const wf_status typed_status = wf_jetstream_event_parse_typed(
                event.json, event.json_len, &typed);
            const bool typed_delete =
                typed_status == WF_OK &&
                typed.commit.operation == WF_JETSTREAM_COMMIT_DELETE &&
                typed.commit.collection != nullptr && typed.commit.rkey != nullptr;
            if (typed_delete) {
                JetstreamEvent js;
                js.source_uri = "at://" + std::string(event.did) + "/" +
                                typed.commit.collection + "/" + typed.commit.rkey;
                js.author_did = event.did;
                js.seq = protocol_v2_ ? event.seq : event.time_us;
                if (typed_status == WF_OK && typed.commit.rev != nullptr) {
                    js.repo_revision = typed.commit.rev;
                    if (!protocol::is_tid(js.repo_revision))
                        js.verification = protocol::Verification::Rejected;
                }
                js.deleted = true;
                on_event(js);
            }
            wf_jetstream_event_typed_free(&typed);
            SyncObservation observation;
            const bool normalized_v2_commit =
                protocol_v2_ && typed_status != WF_OK;
            const bool extracted =
                !typed_delete &&
                (normalized_v2_commit || typed_status == WF_OK) &&
                atperson::extract_jetstream_commit(event.json, event.json_len,
                                                   self_did_, observation);
            if (extracted) {
                JetstreamEvent js;
                js.source_uri = observation.source_uri;
                js.author_did = observation.author_did;
                js.created_at = observation.created_at;
                js.text = observation.text;
                js.reply_root = observation.context.reply_root_uri;
                js.reply_parent = observation.context.reply_parent_uri;
                js.quote_uri = observation.context.quote_uri;
                js.policy_reason = observation.policy_reason;
                js.seq = protocol_v2_ ? event.seq : event.time_us;
                if (typed_status == WF_OK && typed.commit.rev != nullptr) {
                    js.repo_revision = typed.commit.rev;
                    if (!protocol::is_tid(js.repo_revision))
                        js.verification = protocol::Verification::Rejected;
                }
                on_event(js);
            }
        } else if (event.did != nullptr && event.json != nullptr) {
            JetstreamEvent protocol_event;
            protocol_event.author_did = event.did;
            protocol_event.seq = protocol_v2_ ? event.seq : event.time_us;
            protocol_event.protocol_only = true;
            protocol_event.protocol_payload.assign(event.json, event.json_len);
            switch (event.kind) {
            case WF_JETSTREAM_EVENT_SYNC:
                protocol_event.event_type = "#sync";
                break;
            case WF_JETSTREAM_EVENT_IDENTITY:
                protocol_event.event_type = "#identity";
                break;
            case WF_JETSTREAM_EVENT_ACCOUNT:
                protocol_event.event_type = "#account";
                break;
            case WF_JETSTREAM_EVENT_ACCOUNT_DELETE:
                protocol_event.event_type = "#account-delete";
                break;
            default:
                protocol_event.event_type = "#unknown";
                break;
            }
            on_event(protocol_event);
        }

        const std::int64_t cursor = protocol_v2_ ? event.seq : event.time_us;
        if (cursor > 0) {
            cursor_ = cursor_from_seq(cursor);
        }

        wf_jetstream_event_free(&event);
    }

    /* Budget exhausted: the connection stays open for the next batch. */
    return result;
}

} // namespace atperson
