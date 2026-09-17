#ifndef ATPERSON_ATPROTO_CLIENT_HPP
#define ATPERSON_ATPROTO_CLIENT_HPP

#include "engine.hpp"
#include "session.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace atperson {

/* A timeline fetch failed at the HTTP layer (the service rejected the
 * request, e.g. an expired/invalid cursor). The response body has already
 * been released; `status` is the HTTP code. */
class TimelineHttpError : public std::runtime_error {
  public:
    TimelineHttpError(long status, const std::string &message);
    [[nodiscard]] long status() const noexcept { return status_; }

  private:
    long status_;
};

class AtprotoClient {
  public:
    AtprotoClient(std::string service, std::string identifier,
                  std::string app_password);

    /* Authenticated DID for the session (never the login handle). */
    [[nodiscard]] std::string account_did() const;

    /* Fetch one timeline page starting at `cursor` (head when nullopt).
     * Throws TimelineHttpError when the service rejects the request —
     * including a rejected persisted cursor — so the caller can reset to the
     * head and rely on ledger dedup. */
    [[nodiscard]] SyncPage fetch_timeline_page(
        const std::optional<std::string> &cursor, int limit = 50);

  private:
    WolframSession session_;
};

} // namespace atperson

#endif
