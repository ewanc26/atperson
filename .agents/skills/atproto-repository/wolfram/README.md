# wolfram/README.md — atproto-repository mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76`. Headers:
`include/wolfram/repo.h` (umbrella over `repo/{cbor,cid,car,commit,mst,record,diff}.h`),
`include/wolfram/sync.h`, `include/wolfram/sync_verify.h`,
`include/wolfram/sync_subscribe.h`, `include/wolfram/tid.h`,
`include/wolfram/syntax.h`.

## Entry headers

- `repo/car.h` — CAR v1 parse/write, block lookup.
- `repo/cbor.h` — generic canonical DAG-CBOR item parse/serialize; `wf_record_encode_json` for record→CBOR (write path).
- `repo/cid.h` — CID type used throughout (see `atproto-cid`).
- `repo/mst.h` — MST node parse/build, find/add/delete, walks, proofs.
- `repo/commit.h` — commit parse and (write-path) creation.
- `repo/record.h` — record-level repo ops and batched applyWrites (write path).
- `repo/diff.h` — whole-repo verify, import, diff verify/apply, op inversion.
- `sync.h` — `com.atproto.sync.*` downloads (getRepo/getRecord/getBlocks/getBlob/listBlobs/getHead/…).
- `sync_verify.h` — full firehose-commit verification (`wf_sync_verify_commit`).
- `sync_subscribe.h` — `com.atproto.sync.subscribeRepos` firehose client/decode.
- `tid.h` — TID generation/encoding (rkey machinery).
- `syntax.h` — `wf_syntax_record_key_is_valid`, `wf_syntax_aturi_parse`, `wf_syntax_tid_is_valid`.

## Key functions (read-only atperson surface)

- `wf_status wf_car_parse(const unsigned char *data, size_t len, wf_car *out)` / `void wf_car_free(wf_car*)` — parse a CAR into `roots[]` + `blocks[]`; `wf_car_find_block(car, &cid)` looks up a block.
- `wf_status wf_car_write(const wf_car *car, unsigned char **out, size_t *out_len)` — CAR serialization.
- `wf_status wf_repo_verify(const wf_car *car, const wf_repo_verify_options *options, wf_commit *out_commit)` — whole-repo integrity: checks expected DID/signing-key/prev, block CIDs, MST structure. **Run this before any `wf_mst_*` walk on fetched data.**
- `wf_status wf_repo_import(const unsigned char *bytes, size_t len, const wf_repo_verify_options *options, wf_car *out_car, wf_commit *out_commit)` — parse + verify a downloaded export in one call.
- `wf_repo_diff_{verify,apply}` — incremental CAR verification (`wf_sync_verify_diff_car` wraps the verify path with a base repo) and application.
- `wf_sync_get_repo(client, did, since, wf_car *out)` / `wf_sync_get_record`, `wf_sync_get_blocks`, `wf_sync_get_blob`, `wf_sync_get_head`, `wf_sync_get_latest_commit` — the sync XRPCs. Owned outputs freed with `wf_car_free` / `wf_sync_*_free`.
- `wf_status wf_sync_verify_commit(const wf_subscribe_commit *commit, wf_xrpc_client *client, int *out_verified, wf_commit *out_commit)` — firehose path: parses `commit->blocks` CAR, resolves the DID for the signing key, verifies signature + block integrity + MST. Returns `WF_OK` even when the signature fails (`*out_verified == 0`). **Check the flag.**
- `wf_subscribe_start(wf_subscribe_options*, wf_subscribe_handle **out)` / `wf_subscribe_decode_frame` / `wf_subscribe_stop` / `wf_subscribe_event_free` — firehose client and frame decode. `wf_subscribe_commit` carries `seq`, `did`, `commit_cid`, `rev`, `since`, `blocks`, `ops`, `time`, `prev_data` (+`has_prev_data`).

### MST (post-verification only)

`wf_status wf_mst_find(car, root_cid, key, key_len, wf_cid *out)` — record lookup once integrity is proven (Wolfram documents: never on an unverified remote `car`). `wf_mst_paths` (records of one collection), `wf_mst_list` (all leaves), `wf_mst_get_covering_proof`, `wf_mst_cids_for_path` (single-key inclusion/non-inclusion proof, getRecord). `wf_mst_node_parse` enforces depth ≤ `WF_MST_MAX_DEPTH` (1024).

### Commit (read surface)

`wf_status wf_commit_parse(const unsigned char *cbor, size_t len, wf_commit *out)` — commit shape `{ did, version, data, rev, prev, has_prev, cid, sig }`. `has_prev` is how Wolfram represents `prev: null` (genesis) vs omitted.

### TID (rkey machinery, read-only use)

`wf_tid_now(char out[15])`, `wf_tid_from_time`, `wf_tid_encode`, `wf_tid_decode`, `wf_tid_timestamp_micros`, `wf_tid_clockid` — `WF_TID_LEN 13`. Not needed for ingestion, but identifies rkey shapes. (`wf_syntax_tid_is_valid` validates.)

## Ownership / RAII

- `wf_car` owns its roots/blocks — free with `wf_car_free`. `wf_repo_import`'s `out_car` too.
- `wf_repo_diff` owns operations, removed CIDs, and `new_blocks` — free with `wf_repo_diff_free`.
- `wf_subscribe_handle` is opaque — stop with `wf_subscribe_stop`, events freed with `wf_subscribe_event_free`.
- Heap string outputs from sync functions (`wf_sync_head.root/rev`, `wf_sync_blob_list.cids`, …) use the matching `wf_sync_*_free`.
- In `src/app/`, wrap handles/owned structs in RAII shells; never leak `wf_car`/`wf_repo_diff` through a `NetworkObservation`.

## Layer split (C23 vs C++23)

- **C23 core** — never sees CAR/MST/commit/diff types or CIDs. Only distilled observations.
- **C++23 runtime** — owns all Wolfram handles. A future ingestion loop must: (1) persist a durable `seq`/`rev`/cursor before starting (AGENTS.md), (2) verify via `wf_repo_verify`/`wf_sync_verify_commit`, (3) keep every call read-only.

## Offline-testable surface

`wf_car_parse`/`wf_car_write`, `wf_cbor_parse`/`serialize`, `wf_cid_of_block`/`of_bytes`, `wf_commit_parse`, `wf_mst_node_parse`/`wf_mst_key_layer`, `wf_tid_*`, `wf_syntax_*` — all usable in `build-core` without network. Test vectors live in `shared/test-vectors.md`.

## Gaps

Wolfram ships **no** did:webvh, and no BLAKE3/BDASL CID support (repo CIDs are SHA-256 only). Write surfaces (`wf_commit_create`, `wf_repo_create_record`, `wf_repo_apply_writes`, `wf_car_write` into authored repos) exist in Wolfram but atperson must not call them from learning pathways (read-only network rule).