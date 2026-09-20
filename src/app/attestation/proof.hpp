#ifndef ATPERSON_ATTESTATION_PROOF_HPP
#define ATPERSON_ATTESTATION_PROOF_HPP

#include "attestation/signer.hpp"

#include <string>

namespace atperson {

struct AttestationProof {
    std::string payload_cid;
    std::string signature_hex;
    std::string public_key_did;
    std::string key_id;
    std::string algorithm;
};

/* Sign Wolfram's canonical repository-bound payload. The returned object
 * contains only public/audit-safe metadata; private key bytes and payload
 * buffers are released before return. */
[[nodiscard]] AttestationProof create_attestation_proof(
    const AttestationSigner &signer, const std::string &record_json,
    const std::string &metadata_json, const std::string &repository_did);

} // namespace atperson

#endif
