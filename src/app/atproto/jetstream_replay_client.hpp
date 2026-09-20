#ifndef ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP
#define ATPERSON_ATPROTO_JETSTREAM_REPLAY_CLIENT_HPP

#include "jetstream_client.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

typedef struct wf_agent wf_agent;

namespace atperson {

/* Bounded authenticated archive reader. It owns no transport or ingestion
 * state; the caller checkpoints only after this method returns successfully. */
class JetstreamReplayClient {
  public:
    explicit JetstreamReplayClient(wf_agent &agent) noexcept : agent_(agent) {}

    void fetch_window(std::uint64_t after_seq, std::optional<std::uint64_t> before_seq,
                      std::string_view self_did,
                      const std::function<void(const JetstreamEvent &)> &on_event);

  private:
    wf_agent &agent_;
};

} // namespace atperson

#endif
