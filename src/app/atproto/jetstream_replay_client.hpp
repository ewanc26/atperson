#ifndef ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP
#define ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP

#include "jetstream_client.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

typedef struct wf_agent wf_agent;

namespace atperson {

/* Maximum sequence span one operator invocation may ask the archive to plan.
 * Keeping this cap in the replay boundary prevents a missing/incorrect upper
 * bound from turning a bounded ingestion command into an archive sweep. */
inline constexpr std::uint64_t kJetstreamArchiveMaxSequenceSpan = 10'000'000u;

/* Return the exclusive upper bound for a replay request, or nullopt when the
 * requested window is invalid or cannot be represented safely. */
inline std::optional<std::uint64_t> bounded_jetstream_replay_before(
    std::uint64_t after_seq, std::optional<std::uint64_t> before_seq) {
    const std::uint64_t max_before =
        after_seq > std::numeric_limits<std::uint64_t>::max() -
                       kJetstreamArchiveMaxSequenceSpan
            ? std::numeric_limits<std::uint64_t>::max()
            : after_seq + kJetstreamArchiveMaxSequenceSpan;
    if (before_seq) {
        if (*before_seq <= after_seq || *before_seq > max_before) {
            return std::nullopt;
        }
        return before_seq;
    }
    return max_before;
}

struct JetstreamReplayWindow {
    std::uint64_t planned_through_seq{};
    std::uint64_t sealed_tip_seq{};
};

/* Replay source seam: the sync engine depends on this small contract, so
 * restart/truncation behavior can be tested with a scripted source without
 * credentials or network I/O. */
class JetstreamReplaySource {
  public:
    virtual ~JetstreamReplaySource() = default;

    [[nodiscard]] virtual JetstreamReplayWindow fetch_window(
        std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
        std::string_view self_did, const std::vector<std::string> &collections,
        const std::vector<std::string> &dids,
        const std::function<void(const JetstreamEvent &)> &on_event) = 0;
};

/* Bounded authenticated archive reader. It owns no transport or ingestion
 * state; the caller checkpoints only after this method returns successfully. */
class JetstreamReplayClient final : public JetstreamReplaySource {
  public:
    JetstreamReplayClient(wf_agent &agent, std::string archive_token)
        : agent_(agent), archive_token_(std::move(archive_token)) {}

    [[nodiscard]] JetstreamReplayWindow fetch_window(
        std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
        std::string_view self_did, const std::vector<std::string> &collections,
        const std::vector<std::string> &dids,
        const std::function<void(const JetstreamEvent &)> &on_event) override;

  private:
    wf_agent &agent_;
    std::string archive_token_;
};

} // namespace atperson

#endif
