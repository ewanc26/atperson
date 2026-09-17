#include "session.hpp"

namespace atperson {

std::runtime_error wolfram_error(std::string_view operation, wf_status status) {
    return std::runtime_error(std::string(operation) + " failed with Wolfram status " +
                              std::to_string(static_cast<int>(status)));
}

WolframSession::WolframSession(std::string service, std::string identifier,
                               std::string app_password)
    : agent_(wf_agent_new(service.c_str())) {
    if (!agent_) {
        throw std::runtime_error("failed to create Wolfram agent");
    }

    const wf_status status = wf_agent_login(agent_.get(), identifier.c_str(), app_password.c_str());
    if (status != WF_OK) {
        throw wolfram_error("AT Protocol login", status);
    }

    const char *did = wf_agent_get_did(agent_.get());
    if (!did || !did[0]) {
        throw std::runtime_error("AT Protocol login did not yield an account DID");
    }
    did_ = did;
}

} // namespace atperson
