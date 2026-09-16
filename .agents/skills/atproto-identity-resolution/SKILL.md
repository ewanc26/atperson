---
name: atproto-identity-resolution
description: This skill should be used when the user is resolving, validating, parsing, or debugging AT Protocol identities in atperson — handles (domain-name usernames like `alice.bsky.social`) and DIDs (`did:plc:…`, `did:web:…`, `did:webvh:…`) — for the C++23 runtime via Wolfram. Triggers on phrases like "resolve a handle", "why does my author's DID return not-found", "DNS TXT `_atproto.`", "`/.well-known/atproto-did`", "bidirectional verification", "handle.invalid", "`alsoKnownAs` not matching", "DID document", "atproto_pds service endpoint", "signing key multikey", "my PDS location is wrong", "can't find PDS endpoint", "did:web vs did:plc", "com.atproto.identity.resolveHandle", "rotation key", "handle resolution mismatch", "turn this author handle into a DID". Also triggers on Wolfram symbols and headers: `wf_did_resolve`, `wf_did_resolve_raw`, `wf_did_document_parse`, `wf_did_document_free`, `wf_handle_resolve`, `wf_handle_parse_dns_txt`, `wf_did_method_of`, `wf_did_cache_configure`/`wf_did_cache_clear`, `wf_identity_verify_handle`, `wf_agent_resolve_handle`, `wf_agent_verify_handle`, `wf_syntax_handle_is_valid`, `wf_syntax_did_is_valid`, `wf_syntax_at_identifier_is_valid`, and `include/wolfram/identity.h` at the pinned revision 9e63f76. Covers handle syntax rules and reserved TLDs; DNS TXT + HTTPS well-known handle resolution; input normalization (`at://`, `@` prefixes); DID method selection (plc, web — webvh is syntax-recognized but NOT resolvable by Wolfram and must fail loudly, never silently downgrade to did:web); DID document requirements (`Multikey` with `#atproto` suffix, `AtprotoPersonalDataServer` service); bidirectional verification; `handle.invalid` semantics; DID-cache policy. Identity mechanics are owned by Wolfram in atperson — the runtime calls `wf_*`, it does not ship a resolver. Does NOT cover did:plc operation-log cryptography / key rotation internals (see `atproto-repository` and `wolfram/plc.h`), OAuth (see `atproto-oauth`), CID computation (see `atproto-cid`), or parsing `at://` URIs *inside records* (see `atproto-lexicon`).
version: 0.2.0
---

# AT Protocol Identity Resolution in atperson

Every AT Protocol account is a DID; most accounts also have a human-friendly handle. Turning either one into the other — and trusting the result — is the bedrock operation this skill covers. In atperson this happens in the C++23 runtime through Wolfram's `identity.h`; the learning graph only ever consumes the resulting author DID string.

## When to use

Resolving or validating the author of a timeline post for observation provenance, debugging a `did:plc`/`did:web` fetch that returns not-found, checking a `handle.invalid` in feed data, or reasoning about handle ↔ DID binding during ingestion.

## What this skill provides

- Layer routing: identity resolution lives in the C++23 runtime via Wolfram; the C23 core receives only the distilled DID string.
- Wolfram mapping: `identity.h` entry points, ownership/cache semantics, latest-revision gaps (webvh, `alsoKnownAs` exposure).
- Protocol ground truth: handle/DID syntax, resolution flow, document requirements, bidi verification.

## Layer routing

- **C23 core (`include/atperson/`, `src/core/`)** — consumes opaque `author_did` strings in observations (`NetworkObservation.author_did`). No DID/handle types, no resolution mechanics, no Wolfram types. When a handle was resolved by the runtime, only the resulting DID string (or `handle.invalid`) lands in the graph.
- **C++23 runtime (`src/app/`)** — the only layer that resolves identities. `AtprotoClient` currently trusts PDS-provided author DIDs from the feed; when handle-aware resolution is added, it goes through `wf_agent_resolve_handle` / `wf_handle_resolve` + `wf_did_resolve`, wrapped RAII-style.
- **Wolfram (pinned revision `include/wolfram/identity.h`)** — all resolution, parsing, DNS-TXT interpretation, and DID caching. Never re-implement a resolver (AGENTS.md).

## Defaults

- **Resolution pipeline** — normalize → classify → resolve handle → DID → fetch + validate document. Wolfram performs steps 1–3 (`wf_handle_resolve`) and document fetch (`wf_did_resolve`) for you. Full normative rules: `shared/handle-spec.md`, `shared/did-spec.md`. Sequence: `shared/resolution-flow.md`. Fixtures: `shared/test-vectors.md`.
- **Classification** — `did:web:` before `did:plc:` before handle; `wf_did_method_of` returns `WF_DID_METHOD_UNKNOWN` for anything else (including `did:webvh:`). Unknown method = reject loudly; never downgrade to a web/plc fetch.
- **Handle → DID** — Wolfram tries DNS TXT (`_atproto.<handle>`, via `wf_handle_parse_dns_txt`) first, then falls back to `/.well-known/atproto-did`. (Sequential, DNS-first — a documented policy; see `shared/handle-spec.md` for the concurrent options.)
- **Document requirements** — `#atproto` `Multikey` with `publicKeyMultibase` + `#atproto_pds` service; Wolfram's trimmed `wf_did_document` surfaces `pds_endpoint`, `feedgen_endpoint`, `signing_key`, `notif_endpoint`, `method`. Full `alsoKnownAs` requires `wf_did_resolve_raw`.
- **Bidi verification** — `alsoKnownAs` must contain `at://<handle>`. Wolfram's trimmed document does not carry `alsoKnownAs`, so use `wf_did_resolve_raw` for the check, or `wf_identity_verify_handle` / `wf_agent_verify_handle` (XRPC `com.atproto.identity.verifyHandle` answers handle→DID→back).
- **`handle.invalid`** — only for structural failures (bidi mismatch, invalid syntax, empty result after retries), never for transient outages.

## Reading guide

For every identity task:

1. Read `shared/handle-spec.md` and `shared/did-spec.md` first — the normative rules Wolfram implements and your reasoning depends on.
2. Read `wolfram/README.md` for the API mapping at the pinned revision.
3. For runtime integration, read `src/app/atproto_client.hpp/.cpp` to see where author DIDs enter observations.

Decide layer placement before writing code: identity state never enters `src/core/`.

## Common pitfalls

- **`did:webvh:` inputs** — Wolfram has no webvh resolver (`wf_did_method_of` → `UNKNOWN`). Failure is correct; a silent `did:web` downgrade is a security regression.
- **`alsoKnownAs` not in `wf_did_document`** — the trimmed struct only keeps pds/feedgen/signing-key/notif. Do not conclude bidi-failure from that struct alone; use `wf_did_resolve_raw` or `verifyHandle`.
- **DNS returns two `did=…` records with different values** — reject and log; don't pick one. `wf_handle_parse_dns_txt` enforces exactly-one.
- **Bare `handle.invalid` from a transient outage** — retry with backoff before latching (Wolfram's DID cache stale-while-revalidate is built for this; see `wf_did_cache_configure`).
- **Caching policy** — configure `wf_did_cache_configure(stale, max)` at startup if defaults (1h stale / 1d max, reference behavior) don't fit ingestion; call `wf_did_cache_clear` in tests.
- **PDS endpoint with a path/userinfo/query segment** — non-conformant; reject. Wolfram's service resolution only returns valid scheme+host+port endpoints.
- **Freeing owned outputs** — `wf_did_document_free(doc)` for `wf_did_resolve`/`wf_did_document_parse`; plain `free()` for `wf_handle_resolve`/`wf_did_resolve_raw`/`wf_did_resolve_service*` string outputs.

## Decision rules

- **Use a PDS-provided DID over re-resolution?** Yes. Feed JSON already carries `author.did`; only resolve when the identifier is a handle or the DID needs document-level validation.
- **Cache successful resolutions?** Yes — Wolfram's cache defaults (1h/1d) match reference stale-while-revalidate. Cache failed resolutions for seconds-to-minutes only.
- **Emit `handle.invalid` eagerly?** No — structural failures only.
- **Accept a path-based `did:web:example.com:users:alice`?** Not in atproto. Reject.

## Verification

Offline, no network:

```
# syntax classification — returns WF_DID_METHOD_PLC / WEB / UNKNOWN
wf_did_method_of(did)
# string validation
wf_syntax_handle_is_valid(handle)   wf_syntax_did_is_valid(did)
# document invariants from an embedded didDoc (createSession) JSON
wf_did_document_parse(json, len, &doc);
```

Core-only build/tests:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Directory layout

```
atproto-identity-resolution/
├── SKILL.md                    # this file — router
├── shared/
│   ├── handle-spec.md          # normative handle rules
│   ├── did-spec.md             # normative DID rules + document shape
│   ├── resolution-flow.md      # end-to-end sequence
│   └── test-vectors.md         # fixtures
└── wolfram/
    └── README.md               # identity.h mapping at the pinned revision
```

## References

- `shared/handle-spec.md`, `shared/did-spec.md`, `shared/resolution-flow.md`, `shared/test-vectors.md`
- `wolfram/README.md`