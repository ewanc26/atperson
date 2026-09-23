#ifndef ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP
#define ATPERSON_ATPROTO_REPOSITORY_VERIFIER_HPP

#include "protocol.hpp"
#include "wolfram/sync_subscribe.h"

#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>

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

/* Verify a complete repository CAR against a resolved DID signing key. */
protocol::Verification verify_signed_repository_car(std::string_view signing_key,
                                                    const unsigned char *car,
                                                    std::size_t car_len);

bool record_repository_commit(protocol::EvidenceLedger &ledger,
                              const wf_subscribe_commit &commit,
                              wf_xrpc_client *client,
                              std::string_view source,
                              std::uint64_t observed_at);

bool record_repository_car(protocol::EvidenceLedger &ledger,
                           std::string_view repo_did,
                           std::string_view revision,
                           std::string_view signing_key,
                           const unsigned char *car,
                           std::size_t car_len,
                           std::string_view source,
                           std::uint64_t sequence,
                           std::uint64_t observed_at);

bool fetch_and_record_bounded_resync(protocol::EvidenceLedger &ledger,
                                     wf_xrpc_client *client,
                                     std::string_view repo_did,
                                     std::string_view revision,
                                     std::string_view signing_key,
                                     std::string_view since,
                                     std::string_view source,
                                     std::size_t max_blocks,
                                     std::size_t max_bytes,
                                     std::uint64_t sequence,
                                     std::uint64_t observed_at);

} // namespace atperson

#endif
