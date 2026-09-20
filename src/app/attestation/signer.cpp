#include "attestation/signer.hpp"

#include <cstdlib>
#include <stdexcept>

namespace atperson {
namespace {

std::string environment_value(const char *name) {
    const char *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

wf_key_type parse_key_type(const std::string &value) {
    if (value == "p256") return WF_KEY_TYPE_P256;
    if (value == "secp256k1") return WF_KEY_TYPE_SECP256K1;
    throw std::runtime_error(
        "ATPERSON_ATTESTATION_KEY_TYPE must be 'p256' or 'secp256k1'");
}

} // namespace

std::optional<AttestationSigner> attestation_signer_from_environment() {
    const std::string hex = environment_value("ATPERSON_ATTESTATION_KEY_HEX");
    if (hex.empty()) return std::nullopt;

    const std::string type = environment_value("ATPERSON_ATTESTATION_KEY_TYPE");
    const std::string key_id = environment_value("ATPERSON_ATTESTATION_KEY_ID");
    if (type.empty() || key_id.empty()) {
        throw std::runtime_error(
            "ATPERSON_ATTESTATION_KEY_HEX requires KEY_TYPE and KEY_ID");
    }

    AttestationSigner signer;
    signer.key_id = key_id;
    const wf_status status = wf_signing_key_from_hex(
        parse_key_type(type), hex.c_str(), &signer.key);
    if (status != WF_OK) {
        throw std::runtime_error("invalid ATPERSON_ATTESTATION_KEY_HEX");
    }
    return signer;
}

} // namespace atperson
