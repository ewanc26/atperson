#include "atproto/repository_verifier.hpp"
#include "wolfram/crypto.h"
#include "wolfram/repo.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <cstdlib>

int main() {
    wf_subscribe_commit commit{};
    std::strncpy(commit.did, "did:plc:repo", sizeof(commit.did) - 1u);
    std::strncpy(commit.rev, "3jui3s7xq2m2a", sizeof(commit.rev) - 1u);

    const auto missing = atperson::verify_repository_commit(commit, nullptr);
    assert(missing.verification == atperson::protocol::Verification::Unverified);
    assert(missing.repo_did == "did:plc:repo");
    assert(missing.revision == "3jui3s7xq2m2a");

    const unsigned char malformed[] = {0x01, 0x02, 0x03};
    commit.blocks = const_cast<unsigned char *>(malformed);
    commit.blocks_len = sizeof(malformed);
    const auto no_transport = atperson::verify_repository_commit(commit, nullptr);
    assert(no_transport.verification == atperson::protocol::Verification::Unverified);
    wf_subscribe_commit malformed_commit = commit;
    std::strncpy(malformed_commit.rev, "not-a-tid",
                  sizeof(malformed_commit.rev) - 1u);
    const auto rejected = atperson::verify_repository_commit(malformed_commit, nullptr);
    assert(rejected.verification == atperson::protocol::Verification::Rejected);

    wf_signing_key key{};
    if (wf_signing_key_generate(WF_KEY_TYPE_SECP256K1, &key) == WF_OK) {
        unsigned char *car = nullptr;
        size_t car_len = 0;
        wf_car empty{};
        wf_cid root{};
        root.bytes[0] = 0x01;
        root.bytes[1] = 0x71;
        root.bytes[2] = 0x12;
        root.bytes[3] = 0x20;
        root.len = 36;
        wf_commit signed_commit{};
        if (wf_commit_create("did:plc:repo", "3jui3s7xq2m2a", &root, nullptr,
                             &key, &empty, &signed_commit) == WF_OK &&
            wf_car_write(&empty, &car, &car_len) == WF_OK) {
            assert(atperson::verify_signed_repository_car(
                       "did:plc:repo", car, car_len) !=
                   atperson::protocol::Verification::Verified);
            car[car_len - 1u] ^= 0x01u;
            assert(atperson::verify_signed_repository_car(
                       "did:plc:repo", car, car_len) !=
                   atperson::protocol::Verification::Verified);
            std::free(car);
        }
        wf_car_free(&empty);
    }
    const auto path = std::filesystem::temp_directory_path() / "atperson-verifier-evidence.bin";
    std::error_code error;
    std::filesystem::remove(path, error);
    atperson::protocol::EvidenceLedger ledger(path);
    assert(atperson::record_repository_commit(ledger, commit, nullptr,
                                               "wss://relay.example", 42u));
    const auto entries = ledger.entries();
    assert(entries.size() == 1u && entries[0].event_type == "#commit" &&
           entries[0].verification == atperson::protocol::Verification::Unverified);
    std::filesystem::remove(path, error);
    return 0;
}
