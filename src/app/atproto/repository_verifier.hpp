#ifndef ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP
#define ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP

#include "protocol.hpp"
#include "wolfram/sync_subscribe.h"

#include <string>

namespace atperson {

struct RepositoryVerification {
    protocol::Verification verification{protocol::Verification::Unverified};
    std::string repo_did;
    std::string revision;
    std::string signed_root_cid;
};

/* Wolfram owns CAR parsing, DID-key resolution and cryptographic verification.
 * This adapter only translates its result into protocol evidence metadata. */
RepositoryVerification verify_repository_commit(const wf_subscribe_commit &commit,
                                                wf_xrpc_client *client);

} // namespace atperson

#endif
