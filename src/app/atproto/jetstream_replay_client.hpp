#ifndef ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP
#define ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP

#include "jetstream_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

typedef struct wf_agent wf_agent;

namespace atperson {

/* Maximum sequence span one operator invocation may ask the archive to plan.
 * Keeping this cap in the replay boundary prevents a missing/incorrect upper
 * bound from turning a bounded ingestion command into an archive sweep. */
inline constexpr std::uint64_t kJetstreamArchiveMaxSequenceSpan = 10'000'000u;

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
    explicit JetstreamReplayClient(wf_agent &agent) noexcept : agent_(agent) {}

    [[nodiscard]] JetstreamReplayWindow fetch_window(
        std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
        std::string_view self_did, const std::vector<std::string> &collections,
        const std::vector<std::string> &dids,
        const std::function<void(const JetstreamEvent &)> &on_event) override;

  private:
    wf_agent &agent_;
};

} // namespace atperson

#endif
