#include "atproto/repository_verifier.hpp"
#include "wolfram/crypto.h"
#include "wolfram/repo.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <cstdlib>

static bool build_signed_car(const wf_signing_key &key, const char *did,
                             unsigned char **out, size_t *out_len) {
    wf_car car{};
    wf_mst_node node{};
    if (wf_mst_node_build(0, nullptr, nullptr, 0, &node) != WF_OK) return false;
    if (wf_mst_node_finalize(&node, &car) != WF_OK) {
        wf_mst_node_free(&node);
        return false;
    }
    const wf_cid root = node.cid;
    wf_mst_node_free(&node);
    wf_commit commit{};
    if (wf_commit_create(did, "3jui3s7xq2m2a", &root, nullptr, &key, &car,
                         &commit) != WF_OK) {
        wf_car_free(&car);
        return false;
    }
    car.roots = static_cast<wf_cid *>(std::malloc(sizeof(wf_cid)));
    if (!car.roots) {
        wf_car_free(&car);
        return false;
    }
    car.roots[0] = commit.cid;
    car.root_count = 1;
    const wf_status status = wf_car_write(&car, out, out_len);
    wf_car_free(&car);
    return status == WF_OK;
}

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
        char *did_key = nullptr;
        unsigned char *car = nullptr;
        size_t car_len = 0;
        if (wf_signing_key_public_didkey(&key, &did_key) == WF_OK &&
            build_signed_car(key, "did:plc:repo", &car, &car_len)) {
            assert(atperson::verify_signed_repository_car(
                       did_key, car, car_len) ==
                   atperson::protocol::Verification::Verified);
            const auto car_path = std::filesystem::temp_directory_path() /
                                  "atperson-verifier-car-evidence.bin";
            std::error_code car_error;
            std::filesystem::remove(car_path, car_error);
            atperson::protocol::EvidenceLedger car_ledger(car_path);
            assert(atperson::record_repository_car(
                car_ledger, "did:plc:repo", "3jui3s7xq2m2a", did_key, car,
                car_len, "https://pds.example", 9u, 17u));
            assert(car_ledger.entries().size() == 1u &&
                   car_ledger.entries()[0].event_type == "#repository" &&
                   car_ledger.entries()[0].verification ==
                       atperson::protocol::Verification::Verified);
            std::filesystem::remove(car_path, car_error);
            car[car_len - 1u] ^= 0x01u;
            assert(atperson::verify_signed_repository_car(
                       did_key, car, car_len) !=
                   atperson::protocol::Verification::Verified);
            const auto bad_path = std::filesystem::temp_directory_path() /
                                  "atperson-verifier-bad-car-evidence.bin";
            std::filesystem::remove(bad_path, car_error);
            atperson::protocol::EvidenceLedger bad_ledger(bad_path);
            assert(atperson::record_repository_car(
                bad_ledger, "did:plc:repo", "3jui3s7xq2m2a", did_key, car,
                car_len, "https://pds.example", 10u, 18u));
            assert(bad_ledger.entries().size() == 1u &&
                   bad_ledger.entries()[0].event_type == "#car" &&
                   bad_ledger.entries()[0].verification !=
                       atperson::protocol::Verification::Verified);
            std::filesystem::remove(bad_path, car_error);
            std::free(car);
        }
        std::free(did_key);
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
    assert(atperson::fetch_and_record_bounded_resync(
        ledger, nullptr, "did:plc:repo", "3jui3s7xq2m2a", "did:key:zUnsupported",
        "", "https://pds.example", 10u, 1024u, 43u, 43u));
    assert(ledger.entries().back().event_type == "#resync" &&
           ledger.entries().back().verification ==
               atperson::protocol::Verification::Unverified);
    std::filesystem::remove(path, error);
    return 0;
}
