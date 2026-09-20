#ifndef ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP
#define ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP

#include "protocol.hpp"
#include "wolfram/sync_subscribe.h"

#include <string>
#include <string_view>
#include <cstdint>

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

bool record_repository_commit(protocol::EvidenceLedger &ledger,
                              const wf_subscribe_commit &commit,
                              wf_xrpc_client *client,
                              std::string_view source,
                              std::uint64_t observed_at);

} // namespace atperson

#endif
