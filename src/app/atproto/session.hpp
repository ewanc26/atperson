#ifndef ATPERSON_ATPROTO_SESSION_HPP
#define ATPERSON_ATPROTO_SESSION_HPP

// Authenticated Wolfram agent session: the single place atperson establishes
// an AT Protocol login, shared by the read path (timeline fetch) and the
// outbound write path (#25).
//
// At Protocol mechanics belong to Wolfram (root AGENTS.md): this wrapper only
// owns one `wf_agent` RAII handle and the authenticated DID. Credentials are
// construction arguments and are never stored, logged or persisted; only the
// agent handle and the resulting DID survive.
//
// Ownership: the session owns the agent and frees it via the wolfram-cpp RAII
// handle. Callers borrow `agent()` for the duration of a call. Not
// thread-safe: one session is driven by one thread at a time.
//
// Failure modes: constructor throws std::runtime_error when the agent cannot
// be created, the login fails, or no DID is returned.

#include <wolfram/_result.h>
#include <wolfram/agent.h>
#include <wolfram/wolfram.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

class WolframSession {
  public:
    WolframSession(std::string service, std::string identifier, std::string app_password);

    [[nodiscard]] wf_agent *agent() const noexcept {
        return agent_.get();
    }

    /* Authenticated DID for this session (never the login handle). */
    [[nodiscard]] const std::string &did() const noexcept {
        return did_;
    }

  private:
    wolfram::wf_agent_handle agent_;
    std::string did_;
};

/* std::runtime_error naming the operation and the Wolfram status code. */
[[nodiscard]] std::runtime_error wolfram_error(std::string_view operation, wf_status status);

} // namespace atperson

#endif
