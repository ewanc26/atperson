# Outbound record attestation contract

Issue #57 is intentionally separate from the journal-integrity MAC. The MAC
is symmetric operator evidence about a local journal entry; it is not a
record-side signature and must never be presented as one.

## Operator-owned signer

The eventual attestation implementation must receive its signer from an
explicit operator-owned configuration source. The source and key type must be
declared together, for example:

- a configured secret-manager reference, resolved by the runtime;
- a supported private-key encoding and algorithm selected by that reference;
- a stable public key identifier or DID used in audit metadata.

The runtime must not derive a signing key from `ATPERSON_JOURNAL_MAC_KEY`, the
repository DID, record text, an action rkey or any other learned or durable
record field. It must never generate a replacement key implicitly.

Missing, malformed, unsupported or unavailable signer configuration is a hard,
fail-closed error. The outbound record must not be written when attestation is
required and signing cannot complete.

## Signed payload and audit metadata

Wolfram owns canonical repository-bound payload construction. The payload CID
and the signature are produced before the record write, and the exact payload
CID is retained with the outbound audit entry. The audit entry may contain:

- attestation format and version;
- algorithm and public key identifier/DID;
- payload CID;
- signature encoding or reference;
- success or a stable failure reason.

It must never contain the private key, secret-manager value, bearer
credential, or a fabricated DID. Verification metadata is not learned state
and is not a substitute for the immutable outbound action document.

Until the signer contract and implementation are present, `publish` remains
MAC-only and documents that fact explicitly.
