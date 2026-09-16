---
name: atproto-oauth
description: This skill should be used when the user is implementing, auditing, or debugging AT Protocol OAuth in atperson — the C++23 runtime's future alternative to legacy app-password login. Covers the authorization flow (PAR / DPoP / PKCE), client metadata documents, permission / scope design, refresh token handling, session storage, and server-side DPoP validation, all routed through Wolfram (`include/wolfram/oauth.h` and `oauth/*.h`). Triggers on phrases like "OAuth client metadata", "client_id as URL", "private_key_jwt", "dpop_bound_access_tokens", "dpop_signing_alg_values_supported", "PAR", "pushed authorization request", "request_uri", "DPoP proof", "DPoP nonce", "invalid_dpop_proof", "ath claim", "htu", "htm", "jkt", "jwk thumbprint", "refresh token race", "token rotation", "invalid_grant", "access_denied", "state store", "session store", "oauth/authorize", "oauth/callback", "oauth/token", "oauth/par", "oauth/revoke", ".well-known/oauth-protected-resource", ".well-known/oauth-authorization-server", "permission-set", "atproto scope", "iss parameter", "RFC 9449", "RFC 9126", "RFC 7636", "RFC 7523", "RFC 9207", "OAuth 2.1", "SameSite=Lax". Also triggers on Wolfram symbols and headers: `wf_oauth_authorization_begin`, `wf_oauth_par`, `wf_oauth_par_with_auth`, `wf_oauth_callback_validate`, `wf_oauth_exchange_code_with_auth`, `wf_oauth_refresh`, `wf_oauth_revoke`, `wf_oauth_pkce_generate`, `wf_oauth_dpop_key_generate`, `wf_oauth_dpop_proof_create`, `wf_oauth_verify_dpop`, `wf_oauth_verify_request`, `wf_oauth_client_metadata_parse`, `wf_oauth_server_metadata_get`, `wf_oauth_discover`, `wf_oauth_authorization_state_create`, `wf_oauth_session_state_create`, `wf_oauth_token_response_parse`, `wf_session_login`, `wf_auth_client_*`, `wf_agent_login`, `include/wolfram/oauth.h`. Use this skill to reason about how an OAuth-authenticated read path would sit inside the C++23 runtime, audit a metadata document, or debug DPoP/token failures. In atperson OAuth is NOT yet active — the network path uses legacy app-password login (`wf_agent_login`), is read-only, and no app passwords are ever logged or persisted (AGENTS.md). The C23 learning graph never touches tokens or credentials. Does NOT cover DID/handle resolution (see `atproto-identity-resolution`), CAR/MST/commit signing (see `atproto-repository`), CID parsing (see `atproto-cid`), or lexicon-record/XRPC validation beyond bearing DPoP'd requests (see `atproto-lexicon`).
version: 0.1.0
---

# AT Protocol OAuth in atperson

AT Protocol OAuth is an **OAuth 2.1** profile with mandatory **PKCE (S256)**, **PAR**, **DPoP**, and URL-based dynamic client registration via a published **client metadata document**. No `client_secret` — confidential clients authenticate to the token endpoint with a `private_key_jwt` assertion (ES256). Public / SPA / native clients authenticate by DPoP proof alone.

In atperson this is a knowledge + future-integration skill: the network path currently logs in with legacy app passwords (`wf_agent_login`) and is strictly read-only. When OAuth is adopted, Wolfram provides the whole surface; this skill routes to it and keeps the learning graph clean of any credential state.

## When to use

Auditing how an OAuth-authenticated read path would be added to `AtprotoClient`, reviewing a client metadata document, debugging DPoP (`invalid_dpop_proof`), token refresh races, or reasoning about scopes for a read-only consumer. Also when the user asks why atperson still uses app passwords.

## What this skill provides

- Layer routing: OAuth is a C++23 runtime concern through Wolfram; the C core never sees tokens/credentials.
- Wolfram mapping: `oauth.h` + `oauth/*.h` entry points, ownership rules, flow-building API.
- Protocol ground truth: entities, flows, client metadata, DPoP, scopes, sessions, security requirements, troubleshooting, vectors.
- atperson policy: no app passwords logged/persisted; no OAuth writes from learning; OAuth adoption must stay read-only.

## Layer routing

- **C23 core (`include/atperson/`, `src/core/`)** — nothing. No credentials, tokens, DPoP state, or session data. The graph consumes observations only.
- **C++23 runtime (`src/app/`)** — owns `wf_agent_handle` and any future `wf_auth_client`/`wf_session` RAII wrappers. Logs in via `wf_agent_login(identifier, app_password)` today; an OAuth path would replace that with `wf_oauth_authorization_begin` → persist `state_json` → `wf_oauth_callback_validate` → `wf_oauth_exchange_code_with_auth` and use `wf_auth_client_*` for queries. Credentials come from runtime config/env only, never from the learning graph, and are never logged.
- **Wolfram (pinned revision `include/wolfram/oauth.h`)** — all OAuth mechanics; never hand-roll PAR/DPoP/PKCE/JWTs.

## Defaults

- **`client_id` is a URL** resolving to a JSON metadata document. URL path/host/protocol must match byte-for-byte between registration, PAR, and authorize.
- **Every access token is DPoP-bound** — `dpop_bound_access_tokens: true` required; each resource request carries a fresh proof with `ath = SHA-256(access_token)` and a per-origin `nonce`.
- **PAR is required** — push the authorize request, redirect the user with only `client_id` + `request_uri`.
- **Scopes start with `atproto`** — layered: `transition:generic`, `identity:handle`, `repo:*`, `rpc:*`, `include:<permission-set>`.
- **The session belongs to the DID** — persist by `sub` (DID), not handle.
- **Identity verification is mandatory** — `sub` → DID document → `#atproto_pds` → matches the PDS → `authorization_servers[0]` → matches the AS. Skip = CSRF window.

Full normative rules: `shared/spec.md`, `shared/flows.md`, `shared/client-metadata.md`, `shared/dpop.md`, `shared/scopes.md`, `shared/sessions.md`, `shared/security-requirements.md`. Fixtures: `shared/test-vectors.md`. Common failures: `shared/troubleshooting.md`.

## Reading guide

For every OAuth task:

1. Read the relevant `shared/*.md` first (`spec.md` + one of `flows.md` / `dpop.md` / `sessions.md` / `client-metadata.md` / `scopes.md`).
2. Read `wolfram/README.md` for the API mapping at the pinned revision.
3. When the task is "should atperson adopt OAuth for a fetch", confirm the read-only constraint and credential-handling rules before designing; see `src/app/atproto_client.cpp` for the current login shape.

## Common pitfalls

- **Refresh race** — concurrent refreshes invalidate each other's refresh token. Wolfram offers `wf_oauth_session_refresh` / `wf_oauth_refresh`; serialize refreshes per DID behind a lock.
- **`htu` normalization** — strip query strings/fragments, elide default ports before minting a DPoP proof. `invalid_dpop_proof` with identical-looking URLs = suspect this.
- **`SameSite=Strict` kills the callback** — OAuth redirects are cross-origin top-level navigations; use `Lax`.
- **Public clients have a 14-day refresh cap, not 180** — silent until day 15 when `invalid_grant` suddenly fires.
- **App-password hygiene** — never log or persist `wf_agent_login`'s app password (AGENTS.md). It is runtime config, not learning state.
- **JWT/JWK private leakage** — when Wolfram's key APIs expose private components, keep `d`/private halves server-side only.
- **Read-only discipline** — OAuth enables authenticated reads; it must never open a write path (posts/replies/moderations) off the learning graph.

## Decision rules

- **OAuth vs app passwords?** App passwords today (already wired via `wf_agent_login`). Adopt OAuth only when a concrete requirement (long-lived session, no stored credential) demands it — and keep it read-only.
- **Where do tokens live?** C++23 runtime memory/config only, in RAII-wrapped Wolfram handles; encrypted at rest if persisted; never in `src/core/`.
- **Which client kind?** Confidential BFF pattern if a backend exists; atperson's `AtprotoClient` is effectively a server-side client, so BFF semantics apply.

## Verification

OAuth mechanics are not exercised in `build-core` (no network login path). Static/gated checks:

- `wf_oauth_client_metadata_parse(json, len, &metadata)` — validate a metadata doc offline (WF_OK).
- `wf_oauth_pkce_generate` / `wf_oauth_pkce_from_verifier` — offline PKCE invariants.
- `wf_oauth_dpop_key_generate` / `wf_oauth_dpop_key_thumbprint` — offline JWK/jkt invariants.

Core-only build/tests:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Directory layout

```
atproto-oauth/
├── SKILL.md                    # this file — router
├── shared/
│   ├── spec.md                 # OAuth 2.1 + AT Proto profile: entities, invariants
│   ├── flows.md                # byte-level wire content for each step
│   ├── client-metadata.md      # metadata document fields, JWKS rules
│   ├── dpop.md                 # RFC 9449 profile
│   ├── scopes.md               # scope grammar, permission sets
│   ├── sessions.md             # pre-flow state + post-flow session rules
│   ├── security-requirements.md# cookies, keys, tokens, SSRF
│   ├── troubleshooting.md      # common failures and diagnosis
│   └── test-vectors.md         # fixtures for conformance
└── wolfram/
    └── README.md               # oauth.h mapping at the pinned revision
```

## References

- `shared/spec.md`, `shared/flows.md`, `shared/client-metadata.md`, `shared/dpop.md`, `shared/scopes.md`, `shared/sessions.md`, `shared/security-requirements.md`, `shared/troubleshooting.md`, `shared/test-vectors.md`
- `wolfram/README.md`

Upstream normative sources:

- <https://atproto.com/specs/oauth> — AT Proto OAuth profile
- <https://atproto.com/specs/permission> — scopes and permission sets
- <https://atproto.com/guides/auth>, <https://atproto.com/guides/about-oauth>, <https://atproto.com/guides/oauth-patterns>, <https://atproto.com/guides/sdk-auth>, <https://atproto.com/guides/permission-requests>, <https://atproto.com/guides/permission-sets> — conceptual guides
- RFC 9449 (DPoP), RFC 7636 (PKCE), RFC 9126 (PAR), RFC 7523 (JWT client auth), RFC 8414 (server metadata), RFC 9207 (`iss`), OAuth 2.1 draft