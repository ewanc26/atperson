#include "atproto/repository_verifier.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>

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
