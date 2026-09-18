#ifndef ATPERSON_JOURNAL_MAC_HPP
#define ATPERSON_JOURNAL_MAC_HPP

// Journal-integrity MAC (issue #57, scoped): a keyed HMAC-SHA256 over a
// canonical description of one executed outbound record, stored in the action
// journal so a later verifier can detect tampering or accidental divergence
// between the journal line and what was actually published.
//
// This is NOT a badge.blue attestation and produces NO record-side signature
// operation. It never touches Wolfram, the writer, or the network path. It is
// a symmetric journal-integrity check only: whoever holds ATPERSON_JOURNAL_MAC_KEY
// can recompute the MAC for any recorded action and compare.
//
// Security model:
// - the key is a secret (recommended 32-byte / 64-hex); it is never logged,
//   persisted or embedded in output — only the first 8 hex characters are
//   recorded as `key_hint`, enough to tell two keys apart without disclosing
//   (or being able to brute-force from) the key itself;
// - an empty key disables MAC creation (nullopt) so runtimes without the env
//   var write byte-identical journal lines;
// - verification fails closed: a missing MAC, a missing key, a digest change
//   (record parameters differed) or a signature mismatch each return a
//   distinct machine-readable reason code;
// - the canonical string includes the SHA-256 of the record text, so the
//   full text never has to be embedded twice.
//
// No learning state, no C23 involvement, no clock. Pure runtime utility.

#include <optional>
#include <string>
#include <string_view>

namespace atperson {

struct JournalMac {
    std::string mode{"hmac-sha256"};
    std::string key_hint; /* first 8 hex chars of the key; identity only */
    std::string sig;      /* hex HMAC-SHA256 over the canonical string */
    std::string digest;   /* hex SHA-256 over the canonical string */
};

/* Canonical MAC input (issue #57): "atperson-journal-mac-v1:" +
 * repo_did + ":" + rkey + ":" + sha256_hex(text) + ":" + created_at. */
[[nodiscard]] std::string compute_journal_mac_canonical_string(
    std::string_view repo_did, std::string_view rkey, std::string_view text,
    std::string_view created_at);

/* Hex SHA-256 of the canonical string. */
[[nodiscard]] std::string compute_journal_mac_digest(
    std::string_view repo_did, std::string_view rkey, std::string_view text,
    std::string_view created_at);

/* Build the MAC for one executed record. Returns nullopt when key_hex is
 * empty (attestation disabled). */
[[nodiscard]] std::optional<JournalMac> create_journal_mac(
    std::string_view key_hex, std::string_view repo_did, std::string_view rkey,
    std::string_view text, std::string_view created_at);

struct JournalMacVerifyResult {
    bool valid{false};
    std::string reason_code; /* valid | missing_key | digest_mismatch |
                                signature_mismatch | no_mac */
    std::string detail;
    std::string recomputed_digest;
};

/* Recompute and compare. Fails closed; each failure class is distinct. */
[[nodiscard]] JournalMacVerifyResult verify_journal_mac(
    const JournalMac &mac, std::string_view key_hex, std::string_view repository_did,
    std::string_view rkey, std::string_view text, std::string_view created_at);

} // namespace atperson

#endif