# wolfram/README.md — atproto-oauth mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76`. Entry header
`include/wolfram/oauth.h` is an umbrella over `oauth/`:

`metadata.h`, `pkce.h`, `dpop.h`, `par.h` (PAR + token exchange + refresh +
revoke), `state.h` (authorization/session state serialization), `callback.h`,
`flow.h` (single-call authorization flow), `verify.h` (server-side DPoP + token
validation). Sessions from `include/wolfram/session.h`; authenticated transport
from `include/wolfram/auth_client.h`.

## Flow building (client side)

1. `wf_oauth_discover(pds_url, &server, &resource, &client_metadata)` — resolve PDS → AS metadata; or `wf_oauth_server_metadata_get` / `wf_oauth_resource_metadata_get`.
2. Validate what you fetched: `wf_oauth_client_metadata_parse(json, len, &metadata)`, `wf_oauth_server_metadata_parse`, `wf_oauth_resource_metadata_parse` (no network; usable in tests).
3. `wf_oauth_authorization_begin(...)` — takes transport + auth opts (PAR + PKCE + DPoP + state) and returns `authorization_url` plus serialized `state_json` for the pre-flow store. Or drive the steps by hand: `wf_oauth_pkce_generate`, `wf_oauth_par[_with_auth]` (v2 yielded `request_uri`), `wf_oauth_authorization_state_create`/`..._serialize`.
4. `wf_oauth_callback_validate(transport, server, opts, &result)` — check `state`, `iss`, `code`; free result with `wf_oauth_callback_result_free`.
5. `wf_oauth_exchange_code[_with_auth](...)` → tokens; `wf_oauth_token_response_parse` + `wf_oauth_token_response_validate_subject(sub, PDS, ...)` — **identity verification lives here, do not skip.**
6. Persist by DID: `wf_oauth_authorization_state_parse`/`wf_oauth_session_state_serialize` give you copyable session state for storage.
7. Requests ride on `wf_auth_client_*`: `wf_auth_client_new`, `wf_auth_client_ensure_valid` (refresh when needed), then `wf_auth_client_query`/`query_params`/`procedure` (which attach a fresh DPoP proof + `ath` automatically — same `wf_response`/`wf_response_free` contract as `wf_xrpc_*`).

## Key entry points

- OAuth lifecycle: `wf_oauth_authorization_begin` / `_complete`, `wf_oauth_authorization_url_create`, `wf_oauth_exchange_code`, `wf_oauth_refresh`, `wf_oauth_refresh_with_auth`, `wf_oauth_revoke`, `wf_oauth_session_refresh`, `wf_oauth_session_state_{create,serialize,parse,free}`.
- PAR: `wf_oauth_par`, `wf_oauth_par_with_auth`, `wf_oauth_par_response_parse`, `wf_oauth_par_response_free`, `wf_oauth_client_auth_validate`.
- DPoP keys: `wf_oauth_dpop_key_{generate,import,export,thumbprint,free}`, `wf_oauth_dpop_jwk_json`, `wf_oauth_dpop_proof_create` (manual proof), `wf_oauth_client_assertion_create` (private_key_jwt).
- Server-side validation: `wf_oauth_verify_request`, `wf_oauth_verify_dpop`, `wf_oauth_verify_bearer`, `wf_oauth_verify_client_assertion`, `wf_oauth_trusted_keys_{new,add_jwk,free}`, `wf_oauth_dpop_replay_cache_{new,mark_seen,is_seen,free}`, `wf_oauth_verified_token_free`, `wf_oauth_client_assertion_verified_free`.
- Alternate session model: `wf_session_new` + `wf_session_login` / `wf_session_login_with_opts` (app-password login lives here — **atperson's current path**), `wf_session_resume`, `wf_session_refresh`, `wf_session_has_session`, `wf_session_get`, `wf_session_delete`, `wf_session_free`.

## Ownership / RAII

- All OAuth structs heap-owned with dedicated frees: `wf_oauth_token_response_free`, `wf_oauth_client_metadata_free` (server/resource variants too), `wf_oauth_par_response_free`, `wf_oauth_authorization_*`_result_free, `wf_oauth_callback_result_free`, `wf_oauth_dpop_key_free`, `wf_oauth_string_free` (for string-typed outputs that are freed this way). Wrap each in RAII in `src/app/`.
- `wf_auth_client_free`, `wf_session_free` close the bigger handles.
- Do not store `state_json`/session state in C23 — the graph never serializes credentials.

## Layer split (C23 vs C++23)

- **C23 core** — nothing. No tokens, no DPoP keys, no session state, no `wf_oauth_*`.
- **C++23 runtime** — owns the OAuth surface (RAII wrappers around `wf_auth_client`/`wf_session`), performs `wf_oauth_authorization_begin`/callback/exchange, validates `sub`, and feeds authenticated reads into the current extraction path. App-password login (`wf_agent_login`/`wf_session_login`) stays the active mechanism; the app password is runtime config, never logged/persisted, per AGENTS.md.

## Offline-testable surface

`wf_oauth_client_metadata_parse`, `wf_oauth_server_metadata_parse`, `wf_oauth_resource_metadata_parse`, `wf_oauth_pkce_generate`, `wf_oauth_pkce_from_verifier`, `wf_oauth_dpop_key_thumbprint`, `wf_oauth_session_state_{serialize,parse}` round-trip, `wf_oauth_authorization_state_{serialize,parse}` — all network-free; usable in `build-core` tests with the `shared/test-vectors.md` fixtures.

## Gaps

- OAuth is not adopted in atperson yet; there is no session/cookie layer in `src/app/`; `SameSite`/cookie concerns from `shared/security-requirements.md` apply only if a browser-facing callback is ever added.
- `wf_oauth_token_response_validate_subject` covers the `sub`/PDS match; handle re-validation on top is `wf_identity_verify_handle` (see `atproto-identity-resolution`).
- Server-side verify helpers exist for an AS/resource role atperson does not play; listed for completeness only.
- atperson is read-only: `wf_auth_client_procedure` + `wf_agent_*` write actions (`wf_agent_apply_writes`, `wf_agent_create_record`, `wf_agent_delete_post`, …) must never be reachable from learning flows.