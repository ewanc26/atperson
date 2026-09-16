---
name: atproto-attestation
description: This skill should be used when the user is reasoning about AT Protocol record attestations per the badge.blue specification — inline (embedded ECDSA signature) or remote (content-addressed strongRef to a proof record) — in an atperson context. Triggers on phrases like "sign an atproto record", "inline attestation", "remote attestation", "badge.blue", "attest a record", "record signatures array", "`$sig` metadata", "content CID for signing", "com.atproto.repo.strongRef in signatures", "proof record", "verify an attestation", "attestation CID mismatch", "low-S signature", "ECDSA r‖s", "IEEE P1363", "signature normalization", "did:key signing", "replay protection", "cross-repo replay", "signatures[] append", and error strings from the reference crate like `error-atproto-attestation-*`, `UnsupportedKeyType`, `RemoteAttestationCidMismatch`, `SignatureValidationFailed`. Covers the CID-first signing model (DAG-CBOR of `record + $sig(repository)` → SHA-256 → CIDv1 → sign), low-S normalization for P-256 and K-256, the P-384 normalization gap in the reference crate, the two-CID distinction in remote attestations (content CID inside proof record vs proof record's DAG-CBOR CID inside strongRef), and the fact that Wolfram ships no attestation machinery. In atperson this skill is knowledge-only: the network path is read-only, the C core must not gain crypto dependency, and Wolfram exposes only the bsky-native `app.bsky.graph.verification` record lexicon (`WF_LEX_APP_BSKY_GRAPH_VERIFICATION_NSID`) — a display-name verification record, NOT the badge.blue signatures format. Does NOT cover general atproto record parsing or XRPC invocation (see `atproto-lexicon`), DID/handle resolution internals (see `atproto-identity-resolution`), CAR/MST/commit signing (see `atproto-repository`), OAuth token flows (see `atproto-oauth`), or CID parsing/construction for non-attestation records (see `atproto-cid`).
version: 0.1.0
---

# AT Protocol record attestations (badge.blue)

CID-first record attestations as specified at <https://badge.blue/> and implemented in the `atproto-attestation` Rust crate. In atperson this skill is knowledge-only: it exists so agents can reason about attestation-shaped records they may see, and to keep anyone who proposes adding attestation support honest about what Wolfram does and does not provide.

## atperson positioning

- **Read-only network** — atperson never creates attestation records or proof records.
- **No crypto in the C core** — the C23 core owns learned state and algorithms; it must not gain an ECDSA/DAG-CBOR/attestation dependency to (hypothetically) verify signatures.
- **Wolfram gap** — Wolfram ships NO attestation machinery. There is no `wf_*` for the badge.blue `$sig`/`signatures[]` pipeline, no ECDSA signing, no attestation verification, and no content-CID-for-signing helper. Wolfram's only related surface is the bsky-native lexicon `app.bsky.graph.verification` (`WF_LEX_APP_BSKY_GRAPH_VERIFICATION_NSID`, kind record) — a plain record declaring `subject` (DID) + `handle` + `displayName` + `createdAt`. That is NOT the badge.blue signatures format and shares nothing with it algorithmically.
- If the user asks to implement/create/verify attestations, answer from the reference crate + shared specs (this is external tooling territory), and flag that atperson itself has no attestation path.

## Defaults

An attestation binds a cryptographic or content-addressed claim to a specific record in a specific repository:

- **Content CID**: CIDv1, codec `0x71` (dag-cbor), hash `0x12` (SHA-256), 32-byte digest, 36-byte binary form. Computed over a canonical merge of `record` (without `signatures`) + `$sig` metadata (without `cid`/`signature`, with `repository` inserted).
- **Inline attestation**: the 36-byte CID bytes are ECDSA-signed (P-256 or K-256), normalized to low-S, base64-encoded (standard alphabet with padding), and embedded in `record.signatures[]` alongside the metadata.
- **Remote attestation**: the content CID is written into a separate *proof record* in the attestor's repo; the subject record's `signatures[]` carries a `com.atproto.repo.strongRef` pointing at the proof record.
- **Two CIDs** in the remote case: the **content CID** (inside the proof record's `cid` field, identifies the signing payload) vs the **proof CID** (inside the strongRef's `cid` field, the proof record's own plain DAG-CBOR CID). Confusing them is the most common bug.

Full normative rules: `shared/spec.md`. Step-by-step CID procedure: `shared/cid-computation.md`. Inline flow: `shared/inline-attestation.md`. Remote flow: `shared/remote-attestation.md`. Signature normalization: `shared/signature-normalization.md`. Fixtures: `shared/test-vectors.md`. External reference implementation: the Rust crate `atproto-attestation` (badge.blue).

## Reading guide

For every attestation task:

1. Read `shared/spec.md` for the normative rules (terminology, record shapes, verification order, known gaps).
2. If the task is about computing the CID to sign, read `shared/cid-computation.md`. Every non-trivial implementation bug in this space is at this step.
3. Read the flow-specific spec: `shared/inline-attestation.md` or `shared/remote-attestation.md`.
4. For ECDSA signing / normalization details, read `shared/signature-normalization.md`.
5. For actual implementation, consult the reference Rust crate `atproto-attestation` (docs.rs / source) — there are no per-language guides in this skill.

## Common pitfalls

- **Signing the CID string (`bafyrei…`) instead of the 36-byte binary CID.** Silent interop break.
- **Signing the 32-byte digest instead of the 36-byte CID bytes.** Same class of break.
- **Including `repository` in the stored attestation.** It's a transient input to CID computation only; it must not appear in `signatures[]` entries.
- **Forgetting to strip `signatures` from the record before CID computation.** Every new signature re-signs a stripped version; skip the strip and all signatures invalidate each other.
- **Forgetting to strip `cid`/`signature` from metadata before CID computation.** These fields are outputs, not inputs.
- **DER-encoded signatures.** The spec requires IEEE P1363 `r‖s` (64 bytes for P-256/K-256).
- **Skipping low-S normalization.** Normalize explicitly; cost is negligible.
- **URL-safe base64 for `signature.$bytes`.** Spec uses standard base64 (`+`/`/` + padding).
- **Publishing the attested record before the proof record** — dangling strongRef on hiccup.
- **Using P-384.** The reference crate's normalization rejects it (`UnsupportedKeyType`) — use P-256 or K-256 only.
- **Non-canonical CBOR.** Use strict DAG-CBOR only.

## Decision rules

- **Inline or remote?** Inline if the attestor holds their own key and signs in-process; remote if the attestor is a separate service.
- **Which curve?** P-256 for NIST/FIPS; K-256 for atproto signing-key compatibility. Avoid P-384.
- **Where does `repository` come from at verify time?** The DID of the repo the record was fetched from — hardcoding it silently invalidates every inline signature.
- **Multiple attestations?** Fully supported — each signature is computed over the record with `signatures` stripped.

## Verification

Nothing attestation-related is testable in atperson (no code path). `shared/test-vectors.md` documents fixtures usable in *external* tooling. If an agent is tempted to "add verification to atperson", re-read the positioning section above first.

## Directory layout

```
atproto-attestation/
├── SKILL.md                    # this file — router, knowledge-only for atperson
├── shared/
│   ├── spec.md                 # normative attestation rules (badge.blue)
│   ├── cid-computation.md      # content-CID procedure (bit-exact)
│   ├── inline-attestation.md   # inline flow + record shape
│   ├── remote-attestation.md   # remote flow + two-CID model
│   ├── signature-normalization.md # low-S rules, P-384 gap
│   └── test-vectors.md         # fixtures catalog
└── wolfram/
    └── README.md               # the Wolfram gap + the app.bsky.graph.verification note
```

## References

- `shared/spec.md`, `shared/cid-computation.md`, `shared/inline-attestation.md`, `shared/remote-attestation.md`, `shared/signature-normalization.md`, `shared/test-vectors.md`
- `wolfram/README.md`
- External: <https://badge.blue/> (spec), <https://tangled.org/ngerakines.me/atproto-crates/tree/main/crates/atproto-attestation> (reference crate)

Adjacent skills:

- `atproto-lexicon` — record/XRPC parsing, lexicon validation.
- `atproto-identity-resolution` — DID/handle resolution for `did:key` signing-key normalization.
- `atproto-repository` — CAR/MST/commit signing, DRISL canonical CBOR.
- `atproto-cid` — strict CID profile (`$link`, tag 42, `bafyrei`/`bafkrei`).