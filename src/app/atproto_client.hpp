#ifndef ATPERSON_ATPROTO_CLIENT_HPP
#define ATPERSON_ATPROTO_CLIENT_HPP

#include <wolfram/wolfram.hpp>

#include <string>
#include <vector>

namespace atperson {

struct NetworkObservation {
    std::string text;
    std::string source_uri;
    std::string author_did;
    std::string created_at;
};

class AtprotoClient {
  public:
    AtprotoClient(std::string service, std::string identifier,
                  std::string app_password);

    [[nodiscard]] std::vector<NetworkObservation>
    fetch_timeline(int limit = 50);

  private:
    wolfram::wf_agent_handle agent_;
};

} // namespace atperson

#endif
