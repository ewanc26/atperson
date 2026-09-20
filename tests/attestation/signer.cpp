#include "attestation/signer.hpp"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace {
constexpr const char *kScalar =
    "0000000000000000000000000000000000000000000000000000000000000001";

void clear_environment() {
    unsetenv("ATPERSON_ATTESTATION_KEY_HEX");
    unsetenv("ATPERSON_ATTESTATION_KEY_TYPE");
    unsetenv("ATPERSON_ATTESTATION_KEY_ID");
}
} // namespace

int main() {
    clear_environment();
    assert(!atperson::attestation_signer_from_environment().has_value());

    setenv("ATPERSON_ATTESTATION_KEY_HEX", kScalar, 1);
    bool failed = false;
    try {
        (void)atperson::attestation_signer_from_environment();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);

    setenv("ATPERSON_ATTESTATION_KEY_TYPE", "p256", 1);
    setenv("ATPERSON_ATTESTATION_KEY_ID", "did:key:test#key", 1);
    const auto signer = atperson::attestation_signer_from_environment();
    assert(signer.has_value());
    assert(signer->key.type == WF_KEY_TYPE_P256);
    assert(signer->key.bytes[31] == 1);
    assert(signer->key_id == "did:key:test#key");

    setenv("ATPERSON_ATTESTATION_KEY_TYPE", "invalid", 1);
    failed = false;
    try {
        (void)atperson::attestation_signer_from_environment();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);
    clear_environment();
    std::puts("attestation signer tests passed");
}
