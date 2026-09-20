#include "repository_verifier.hpp"

#include "wolfram/repo/cid.h"
#include "wolfram/repo/car.h"
#include "wolfram/sync.h"
#include "wolfram/sync_verify.h"
#include "wolfram/verify.h"

#include <cstdlib>

namespace atperson {

protocol::Verification verify_signed_repository_car(std::string_view signing_key,
                                                    const unsigned char *car,
                                                    std::size_t car_len) {
    if (signing_key.empty() || car == nullptr || car_len == 0u)
        return protocol::Verification::Rejected;
    int valid = 0;
    const wf_status status = wf_verify_record_commit(
        std::string(signing_key).c_str(), car, car_len, &valid);
    if (status != WF_OK) return protocol::Verification::Unverified;
    return valid ? protocol::Verification::Verified : protocol::Verification::Rejected;
}

RepositoryVerification verify_repository_commit(const wf_subscribe_commit &commit,
                                                wf_xrpc_client *client) {
    RepositoryVerification result;
    result.repo_did = commit.did;
    result.revision = commit.rev;
    if (result.repo_did.empty() || !protocol::is_tid(result.revision)) {
        result.verification = protocol::Verification::Rejected;
        return result;
    }
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

bool record_repository_car(protocol::EvidenceLedger &ledger,
                           std::string_view repo_did,
                           std::string_view revision,
                           std::string_view signing_key,
                           const unsigned char *car,
                           std::size_t car_len,
                           std::string_view source,
                           std::uint64_t sequence,
                           std::uint64_t observed_at) {
    const auto verification = verify_signed_repository_car(signing_key, car, car_len);
    wf_car parsed{};
    std::string root;
    if (car != nullptr && car_len > 0u && wf_car_parse(car, car_len, &parsed) == WF_OK &&
        parsed.root_count > 0u) {
        char *cid = wf_cid_to_string(&parsed.roots[0]);
        if (cid != nullptr) {
            root = cid;
            std::free(cid);
        }
    }
    wf_car_free(&parsed);
    if (verification == protocol::Verification::Verified &&
        !root.empty()) {
        const auto fact = protocol::accept_repository_fact(
            repo_did, revision, root, source, verification);
        if (fact) return protocol::append_repository_fact(ledger, *fact, sequence, observed_at);
    }
    return protocol::append_firehose_event(
        ledger, source, "#car", repo_did,
        std::string(revision) + "|" + (root.empty() ? "no-root" : root), sequence,
        observed_at, verification);
}

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
                                     std::uint64_t observed_at) {
    wf_car fetched{};
    const wf_status status = wf_sync_get_repo(
        client, std::string(repo_did).c_str(),
        since.empty() ? nullptr : std::string(since).c_str(), &fetched);
    if (status != WF_OK) {
        return protocol::append_firehose_event(
            ledger, source, "#resync", repo_did, std::string(revision) + "|fetch-failed",
            sequence, observed_at, protocol::Verification::Unverified);
    }
    bool bounded = fetched.block_count <= max_blocks;
    unsigned char *bytes = nullptr;
    std::size_t length = 0;
    if (bounded && wf_car_write(&fetched, &bytes, &length) == WF_OK)
        bounded = length <= max_bytes;
    bool recorded = false;
    if (bounded && bytes != nullptr) {
        recorded = record_repository_car(
            ledger, repo_did, revision, signing_key, bytes, length, source, sequence,
            observed_at);
    } else {
        recorded = protocol::append_firehose_event(
            ledger, source, "#resync", repo_did,
            std::string(revision) + "|bounds-exceeded", sequence, observed_at,
            protocol::Verification::Rejected);
    }
    std::free(bytes);
    wf_car_free(&fetched);
    return recorded;
}

} // namespace atperson
