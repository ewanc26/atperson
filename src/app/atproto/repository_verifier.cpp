#include "repository_verifier.hpp"

#include "wolfram/repo/cid.h"
#include "wolfram/sync_verify.h"

#include <cstdlib>

namespace atperson {

RepositoryVerification verify_repository_commit(const wf_subscribe_commit &commit,
                                                wf_xrpc_client *client) {
    RepositoryVerification result;
    result.repo_did = commit.did;
    result.revision = commit.rev;
    if (commit.blocks == nullptr || commit.blocks_len == 0u || client == nullptr) {
        return result;
    }

    int valid = 0;
    wf_commit parsed{};
    const wf_status status = wf_sync_verify_commit(&commit, client, &valid, &parsed);
    if (status != WF_OK) {
        result.verification = protocol::Verification::Unverified;
        return result;
    }
    result.verification = valid ? protocol::Verification::Verified
                                : protocol::Verification::Rejected;
    result.repo_did = parsed.did;
    result.revision = parsed.rev;
    char *cid = wf_cid_to_string(&parsed.cid);
    if (cid != nullptr) {
        result.signed_root_cid = cid;
        std::free(cid);
    }
    return result;
}

bool record_repository_commit(protocol::EvidenceLedger &ledger,
                              const wf_subscribe_commit &commit,
                              wf_xrpc_client *client,
                              std::string_view source,
                              std::uint64_t observed_at) {
    const auto verification = verify_repository_commit(commit, client);
    if (!verification.signed_root_cid.empty()) {
        const auto fact = protocol::accept_repository_fact(
            verification.repo_did, verification.revision,
            verification.signed_root_cid, source, verification.verification);
        if (fact) {
            return protocol::append_repository_fact(
                ledger, *fact, commit.seq > 0 ? static_cast<std::uint64_t>(commit.seq) : 0u,
                observed_at);
        }
    }
    return protocol::append_firehose_event(
        ledger, source, "#commit", verification.repo_did,
        verification.revision + "|unverified-or-invalid", commit.seq > 0
            ? static_cast<std::uint64_t>(commit.seq) : 0u, observed_at,
        verification.verification);
}

} // namespace atperson
