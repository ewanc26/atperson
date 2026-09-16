# wolfram/README.md — atproto-identity-resolution mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76` (see CMakeLists.txt
`FetchContent`). Headers: `include/wolfram/identity.h`, `include/wolfram/plc.h`,
`include/wolfram/syntax.h`, `include/wolfram/agent.h`.

## Entry headers

- `include/wolfram/identity.h` — DID/handle resolution, DID document parsing, DID cache.
- `include/wolfram/syntax.h` — offline string validation for handles, DIDs, AT identifiers, `at://` URIs.
- `include/wolfram/plc.h` — did:plc operation construction/signing/submission (write path; read-only for atperson, see Gaps).
- `include/wolfram/agent.h` — session-aware convenience: `wf_agent_resolve_handle`, `wf_agent_verify_handle`.

## Key functions

- `wf_did_method wf_did_method_of(const char *did)` — classify by prefix. `WF_DID_METHOD_UNKNOWN` for anything non-plc/web, **including did:webvh**. No fallback.
- `wf_status wf_handle_resolve(wf_xrpc_client *client, const char *handle, char **out_did)` — DNS TXT `_atproto.<handle>` first, then `/.well-known/atproto-did`. Caller frees `*out_did` with `free()`.
- `wf_status wf_handle_parse_dns_txt(const wf_dns_txt_chunk *chunks, size_t count, char **out_did)` — interprets `wf_dns_txt_chunk`s into the single `did=` value (exactly-one rule). Use when you bring your own DNS adapter; caller frees with `free()`.
- `wf_status wf_did_resolve(wf_xrpc_client *client, const char *did, wf_did_document *out)` — did:plc/did:web → trimmed document. `client` is used purely as HTTP transport (base URL ignored; directory/host derived from the DID). Caller frees with `wf_did_document_free`.
- `wf_status wf_did_resolve_raw(wf_xrpc_client *client, const char *did, char **out_json)` — same dispatch/cache, but returns the **full untrimmed document JSON** (needed for `alsoKnownAs`, full `verificationMethod`/`service` arrays, bidi check). Caller frees with `free()`.
- `wf_status wf_did_document_parse(const char *json, size_t len, wf_did_document *out)` — parse an embedded `didDoc` (e.g. `createSession`/`refreshSession` response) with no network. Caller frees with `wf_did_document_free`.
- `wf_status wf_did_resolve_service(_by_id)(client, did, …)` — extract `AtprotoPersonalDataServer` etc. endpoints; returns only valid scheme+host+port HTTPS URLs, `WF_ERR_NOT_FOUND` otherwise.
- `wf_status wf_did_resolve_verification_key(client, did, key_id, char **out_didkey)` — resolve `#atproto` signing key, normalized to `did:key:z…`. Caller frees with `free()`.
- `void wf_did_cache_configure(time_t stale_ttl_seconds, time_t max_ttl_seconds)` / `void wf_did_cache_clear(void)` — process-wide stale-while-revalidate document cache; unconfigured defaults 1h / 1d (reference `@atproto/identity` behavior); stale served between stale/max when refresh fails; past max, failure propagates.
- `wf_status wf_identity_verify_handle(wf_xrpc_client *client, const char *handle, int *out_valid)` — XRPC `com.atproto.identity.verifyHandle` (handle → DID → `alsoKnownAs` check). Server-side bidi answer.
- Session-aware (agent.h): `wf_agent_resolve_handle`, `wf_agent_verify_handle` — same operations behind a logged-in `wf_agent_handle`.

### `wf_did_document` (trimmed shape)

```c
typedef struct wf_did_document {
    char *did;                    /* "did:plc:…" */
    char *pds_endpoint;           /* AtprotoPersonalDataServer endpoint  */
    char *feedgen_endpoint;       /* BskyFeedGenerator endpoint          */
    char *signing_key;            /* normalized did:key signing key      */
    char *notif_endpoint;         /* BskyNotificationService endpoint    */
    wf_did_method method;
} wf_did_document;
```

**`alsoKnownAs` is NOT retained.** For bidi verification read `wf_did_resolve_raw` JSON (or use `verifyHandle`).

## Ownership / RAII

- `wf_did_document` owns its strings; free always via `wf_did_document_free`. `wf_did_document_free` is NULL-safe.
- String outputs from `wf_handle_resolve`, `wf_handle_parse_dns_txt`, `wf_did_resolve_raw`, `wf_did_resolve_service*`, `wf_did_resolve_verification_key` are heap-allocated and freed with plain `free()`.
- Wrap `wf_agent_handle` in a RAII type in `src/app/` (as `AtprotoClient` does) rather than raw `wf_agent_*` calls at call sites.

## Layer split (C23 vs C++23)

- **C23 core** — receives only opaque `author_did`/`handle.invalid` strings as part of an observation. No DID types, no resolution.
- **C++23 runtime** — the sole resolver. For feed ingestion: trust PDS-provided `author.did`; resolve only when needed (`wf_agent_resolve_handle`, `wf_did_resolve`/`wf_did_resolve_raw`). Configure `wf_did_cache_configure` at startup.

## Offline-testable surface

`wf_did_method_of`, `wf_syntax_*` (all validators), `wf_did_document_parse` (embedded didDoc), `wf_syntax_aturi_parse` — all usable in `build-core` tests without network. `wf_did_resolve`/`wf_handle_resolve` need an `wf_xrpc_client` + network.

## Gaps

- **did:webvh** — not resolvable by Wolfram (`wf_did_method_of` → `UNKNOWN`; no log verifier shipped). Keep failing loudly; wire a webvh resolver only if atperson traffic ever needs it.
- **Concurrent DNS+HTTPS** — Wolfram is sequential DNS-first (matches the Go reference), a documented divergence from the strict-join option in `shared/handle-spec.md`. Acceptable: atperson tolerates the DNS-first policy.
- **`alsoKnownAs`** — not exposed in `wf_did_document`; use `wf_did_resolve_raw` + cJSON for bidi checks.
- **PLC writes, handle changes** — `plc.h`/`wf_identity_update_handle` exist in Wolfram but are write-path. atperson's network path is read-only; do not call them from learning flows.