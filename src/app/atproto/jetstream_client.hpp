/* Jetstream public backfill client (#60).
 *
 * Jetstream is an unauthenticated public firehose: the agent ingests
 * `com.atproto.sync.subscribeRepos` commit frames over a WebSocket without
 * ever logging in. This module owns the transport and the per-cycle budget;
 * it owns no learned state and never writes.
 *
 * The connection is created on the first `fetch_batch` (never in the
 * constructor — the caller may decide not to run) and kept open across
 * successive `fetch_batch` calls so the feed's internal reconnection can
 * resume from its own last-delivered cursor without losing the budget
 * enjoyed in a single custom-reconnect gap.
 *
 * The cursor is the Jetstream envelope microsecond timestamp, an opaque
 * sequence number stored as a decimal string in the ingestion state. It is
 * passed to `wf_jetstream_connect` verbatim and never parsed, compared as
 * meaning, or treated as proof that an observation was learned. An empty
 * cursor means "start at the feed head"; the engine persists the cursor only
 * while it is non-empty.
 *
 * Bounded work: each cycle processes at most `max_events` commit frames and
 * stops after `max_ms` of wall time, so a misbehaving feed cannot starve the
 * daemon. The cursor is checkpointed only after every event in the cycle has
 * been durably handled, matching the engine's ordering invariant.
 *
 * Failure modes: throws std::runtime_error on a fatal transport error
 * (connect failure, parse failure of a frame the feed emitted). A
 * WOULD_BLOCK return from `wf_jetstream_next` (idle socket or reconnect
 * backoff) is not a failure: the caller sleeps for the advertised delay and
 * retries the same batch.
 */

#ifndef ATPERSON_ATPROTO_JETSTREAM_CLIENT_HPP
#define ATPERSON_ATPROTO_JETSTREAM_CLIENT_HPP

#include "engine.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace atperson {

/* One decoded Jetstream frame, already reduced to what the learning core
 * needs. Produced by JetstreamClient in the network build and by test
 * fixtures offline. */
struct JetstreamEvent {
    std::string source_uri; /* at://<did>/app.bsky.feed.post/<rkey> */
    std::string author_did;
    std::string created_at; /* ISO 8601 */
    std::string text;
    std::string reply_root;
    std::string reply_parent;
    std::string quote_uri;
    std::int64_t seq{0};
    bool deleted{false};
};

/* A Jetstream feed: connect, fetch a bounded batch, checkpoint the cursor.
 * The `on_event` callback is invoked once per commit frame that translates to
 * an observation; it must not throw. The client owns the connection and
 * reconnects internally on transport failure, surfacing the delay through
 * `reconnect_after_ms`.
 *
 * The cursor is the Jetstream envelope microsecond timestamp, as a decimal
 * string. An empty string means "start at the feed head". */
class JetstreamClient {
  public:
    explicit JetstreamClient(std::string endpoint,
                             std::string_view collections = "app.bsky.feed.post");
    ~JetstreamClient();
    JetstreamClient(const JetstreamClient &) = delete;
    JetstreamClient &operator=(const JetstreamClient &) = delete;

    /* Fetch at most `limits.max_events` commit frames, calling `on_event`
     * for each one that translates to an observation. Returns the number of
     * events consumed and whether the feed was exhausted (never true in this
     * API — the socket simply goes idle and WOULD_BLOCKs). Throws
     * std::runtime_error on a fatal connect or parse failure. */
    std::pair<std::uint64_t, bool> fetch_batch(
        const JetstreamLimits &limits,
        std::function<void(const JetstreamEvent &)> on_event);

    /* Milliseconds to wait before the next reconnect attempt, or zero when
     * connected and due. Call this after a WOULD_BLOCK fetch_batch to sleep. */
    std::uint32_t reconnect_after_ms() const;

    /* The cursor to persist: the decimal Jetstream sequence number reached
     * after this batch, or empty when nothing has been delivered yet. */
    [[nodiscard]] std::string cursor() const noexcept { return cursor_; }

  private:
    /* Establish (or re-establish) the underlying stream. Throws on failure. */
    void *connect();

    std::string endpoint_;
    std::string collections_;
    std::string cursor_;
    void *impl_{nullptr};
};

} // namespace atperson

#endif