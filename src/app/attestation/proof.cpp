#include "attestation/proof.hpp"

#include "wolfram/plc.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace atperson {
namespace {

std::string hex_encode(const unsigned char *bytes, std::size_t length) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) out << std::setw(2) << (unsigned)bytes[i];
    return out.str();
}

const char *algorithm_name(wf_key_type type) {
    switch (type) {
    case WF_KEY_TYPE_P256:
        return "p256";
    case WF_KEY_TYPE_SECP256K1:
        return "secp256k1";
    default:
        return nullptr;
    }
}

} // namespace

AttestationProof create_attestation_proof(
    const AttestationSigner &signer, const std::string &record_json,
    const std::string &metadata_json, const std::string &repository_did) {
    const char *algorithm = algorithm_name(signer.key.type);
    if (!algorithm || signer.key_id.empty() || repository_did.empty()) {
        throw std::invalid_argument("incomplete attestation signer configuration");
    }

    wf_attestation_payload payload{};
    const wf_status payload_status = wf_attestation_payload_build(
        record_json.c_str(), metadata_json.c_str(), repository_did.c_str(), &payload);
    if (payload_status != WF_OK) {
        throw std::runtime_error("unable to build canonical attestation payload");
    }

    unsigned char signature[64]{};
    char *public_key = nullptr;
    const wf_status sign_status =
        wf_sign(&signer.key, payload.cbor, payload.cbor_len, signature, sizeof(signature));
    const wf_status public_status =
        sign_status == WF_OK ? wf_signing_key_public_didkey(&signer.key, &public_key)
                             : WF_ERR_INVALID_ARG;
    if (sign_status != WF_OK || public_status != WF_OK || public_key == nullptr) {
        wf_attestation_payload_free(&payload);
        std::free(public_key);
        throw std::runtime_error("unable to sign canonical attestation payload");
    }

    char *payload_cid = wf_cid_to_string(&payload.cid);
    if (!payload_cid) {
        std::free(public_key);
        wf_attestation_payload_free(&payload);
        throw std::runtime_error("unable to render attestation payload CID");
    }
    AttestationProof proof;
    proof.payload_cid = payload_cid;
    proof.signature_hex = hex_encode(signature, sizeof(signature));
    proof.public_key_did = public_key;
    proof.key_id = signer.key_id;
    proof.algorithm = algorithm;
    std::free(payload_cid);
    std::free(public_key);
    wf_attestation_payload_free(&payload);
    return proof;
}

} // namespace atperson
