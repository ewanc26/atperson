#ifndef ATPERSON_ATTESTATION_SIGNER_HPP
#define ATPERSON_ATTESTATION_SIGNER_HPP

#include "wolfram/crypto.h"

#include <optional>
#include <string>

namespace atperson {

/* Operator-owned signing material. The private scalar is held only in the
 * process; callers must never serialise this value into journal or audit
 * state. */
struct AttestationSigner {
    wf_signing_key key{};
    std::string key_id;
};

/* Unset ATPERSON_ATTESTATION_KEY_HEX disables optional attestation. If it is
 * set, type and key id are mandatory and malformed configuration throws. */
[[nodiscard]] std::optional<AttestationSigner>
attestation_signer_from_environment();

} // namespace atperson

#endif
