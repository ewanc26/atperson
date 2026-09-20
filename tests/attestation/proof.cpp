#include "attestation/proof.hpp"

#include "wolfram/plc.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
} // namespace

int main() {
    atperson::AttestationSigner signer{};
    signer.key.type = WF_KEY_TYPE_P256;
    signer.key.bytes[31] = 1;
    signer.key_id = "did:key:test#key";
    const std::string record = R"({"$type":"app.bsky.feed.post","text":"hello"})";
    const std::string metadata = R"({"$type":"atperson.attestation.v1","action":"a1"})";
    const std::string repository = "did:plc:example";

    const atperson::AttestationProof proof =
        atperson::create_attestation_proof(signer, record, metadata, repository);
    assert(!proof.payload_cid.empty());
    assert(proof.signature_hex.size() == 128);
    assert(proof.public_key_did.rfind("did:key:z", 0) == 0);
    assert(proof.algorithm == "p256");

    wf_attestation_payload payload{};
    assert(wf_attestation_payload_build(record.c_str(), metadata.c_str(),
                                        repository.c_str(), &payload) == WF_OK);
    unsigned char signature[64]{};
    for (std::size_t i = 0; i < sizeof(signature); ++i) {
        const int high = nibble(proof.signature_hex[i * 2]);
        const int low = nibble(proof.signature_hex[i * 2 + 1]);
        assert(high >= 0 && low >= 0);
        signature[i] = static_cast<unsigned char>((high << 4) | low);
    }
    assert(wf_verify(proof.public_key_did.c_str(), payload.cbor, payload.cbor_len,
                     signature, sizeof(signature)) == WF_OK);
    wf_attestation_payload_free(&payload);
    std::puts("attestation proof tests passed");
}
