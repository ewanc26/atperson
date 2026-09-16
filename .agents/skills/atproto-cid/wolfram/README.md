# wolfram/README.md — atproto-cid mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76` (see CMakeLists.txt
`FetchContent`). Header: `include/wolfram/repo/cid.h`.

## Entry headers

- `include/wolfram/repo/cid.h` — everything CID-related: type, parse/format, compute, incremental hasher, equality.
- `include/wolfram/xrpc.h` — `wf_status` (`WF_OK`, `WF_ERR_INVALID_ARG`, …) used as the return convention.

## Key functions

- `wf_status wf_cid_from_string(const char *str, wf_cid *out)` — parse a base32 CIDv1 string form. Rejects non-CIDv1 (dag-cbor/SHA-256) forms; `WF_ERR_INVALID_ARG` for anything outside the subset. The ingest-time validator for `$link` / blob `ref` strings.
- `char *wf_cid_to_string(const wf_cid *cid)` — render the base32 string form. **Caller frees** with `free()`.
- `wf_status wf_cid_of_block(const unsigned char *cbor, size_t len, wf_cid *out)` — CID of a DAG-CBOR block (dag-cbor 0x71 / SHA-256 0x12).
- `wf_status wf_cid_of_bytes(const unsigned char *bytes, size_t len, wf_cid *out)` — CID of raw bytes (raw 0x55 / SHA-256). This is the blob reference kind.
- `int cid_equal(const wf_cid *a, const wf_cid *b)` — byte equality of the 36-byte binary forms. **Note the unprefixed name** — it is declared in `repo/cid.h`.
- Incremental hashing, bounded memory for large data:
  - `wf_cid_hasher *wf_cid_hasher_new(void)`
  - `wf_status wf_cid_hasher_update(wf_cid_hasher*, const unsigned char*, size_t)`
  - `wf_status wf_cid_hasher_finish_raw(wf_cid_hasher*, wf_cid *out)` — single-use after finish
  - `void wf_cid_hasher_free(wf_cid_hasher*)`

## Ownership / RAII

- `wf_cid` is a plain value struct (`unsigned char bytes[36]; size_t len;`) — pass and return by pointer, no lifetime management.
- `wf_cid_to_string` heap-allocates; free with plain `free()` (not a `wf_…_free`).
- Hasher must be freed with `wf_cid_hasher_free`; wrap it in an RAII guard if `src/app/` uses it.
- Keep Wolfram types (`wf_cid`) inside `src/app/`. `AtprotoClient` currently exposes dist; if it later exposes a CID-bearing observation, prefer distilling to an opaque string/URI before the C23 boundary.

## Layer split (C23 vs C++23)

- **C23 core** — never sees CIDs. The learning graph receives distilled observations (text, source_uri, author_did, created_at) only.
- **C++23 runtime** — the only CID touchpoint. Wolfram does all parsing/computation. Feed JSON from `wf_agent_get_timeline` carries `$link`/ref strings inside `wf_response` JSON; extract with cJSON and validate with `wf_cid_from_string` if a CID needs verification.

## Offline-testable surface

`wf_cid_from_string`, `wf_cid_to_string`, `wf_cid_of_block`, `wf_cid_of_bytes`, `cid_equal`, and the hasher all work with no network. Useful for `build-core` tests (e.g. feed-observation parse tests lose their network dependency when timestamps/CIDs are validated offline).

## Gaps

Wolfram does not implement BDASL (BLAKE3) CIDs — `wf_cid_*` only handle the DASL SHA-256 profile; anything BDASL is out of scope for atperson (no large-file blob ingestion is planned). When you see a `bafkrei`/`bafyrei`-prefixed string, Wolfram's parse covers it; a BLAKE3-tagged multihash will be rejected, which is the desired conservative behavior.