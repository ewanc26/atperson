---
name: atproto-repository
description: This skill should be used when the user is constructing, parsing, exporting, importing, verifying, or signature-validating an AT Protocol user repository in atperson — the durable per-account record store represented as a Merkle Search Tree inside a signed commit, serialized on the wire as a CAR v1 file — via Wolfram. Triggers on phrases like "CAR file", "CAR v1", "car export", "com.atproto.sync.getRepo", "com.atproto.sync.getBlocks", "subscribeRepos", "repo export", "MST", "Merkle Search Tree", "MST node", "left subtree", "fanout", "key height", "repo commit", "commit signature", "sig field", "signing bytes", "UnsignedCommit", "commit version 3", "rev TID", "prev commit", "DAG-CBOR", "DRISL", "canonical CBOR", "map key ordering", "tag 42", "block store", "block CID mismatch", "verify repo", "inductive verification", "prevData", "record CID in MST", "at://<did>/<collection>/<rkey>", "dedup firehose events". Also triggers on Wolfram symbols and entry headers: `wf_repo_verify`, `wf_repo_import`, `wf_repo_diff_verify`, `wf_repo_diff_apply`, `wf_car_parse`/`wf_car_free`, `wf_car_write`, `wf_mst_find`/`wf_mst_add`/`wf_mst_delete`/`wf_mst_list`/`wf_mst_paths`, `wf_mst_get_covering_proof`, `wf_mst_cids_for_path`, `wf_commit_parse`/`wf_commit_create`, `wf_repo_create_record`/`wf_repo_apply_writes`, `wf_sync_get_repo`, `wf_sync_verify_diff_car`, `wf_sync_verify_commit`, `wf_subscribe_start`, `wf_subscribe_decode_frame`, `wf_tid_*`, `include/wolfram/repo.h`, `include/wolfram/sync.h`, `include/wolfram/sync_verify.h`, `include/wolfram/sync_subscribe.h`, `include/wolfram/tid.h`. Use this skill to reason about repo/mirror ingestion, firehose commit verification, diff verification, or to debug "CID mismatch", "unknown codec in CAR", "MST node not found", "signature invalid", "prev null vs absent", "partial tree", "missing block", or "inverted tree root didn't match prevData". In atperson all repo mechanics are owned by Wolfram (`wf_repo_*`, `wf_car_*`, `wf_sync_*`); the C++23 runtime calls them and the C23 core never sees repo types. atperson's network path is read-only: no commit signing or record writes as side effects of learning. Covers DRISL (canonical DAG-CBOR) rules, CAR v1 framing, MST structure + invariant/proofs, commit shape/signing bytes/verification, TID rkeys, and sync/subscribe surfaces. Does NOT cover CID parsing/construction (see `atproto-cid`, `repo/cid.h`), DID resolution / locating the signing key (see `atproto-identity-resolution`), lexicon-level record validation or XRPC invocation (see `atproto-lexicon`), or OAuth (see `atproto-oauth`). Bluesky-record idioms are out of scope.
version: 0.2.0
---

# AT Protocol Repository in atperson

An AT Protocol **repository** is a single account's entire record store, addressed by its DID. Every record lives at a key in a Merkle Search Tree; the tree's root CID is sealed by a signed commit; the commit plus every block it transitively references are the repository. On the wire, it travels as a **CAR v1** file.

## When to use

Reasoning about how repo-level data (CAR exports, firehose commits, diffs) would feed observation ingestion, verifying a firehose commit event's signature/block integrity, deduplicating firehose `seq` before learning, or debugging a CID/signature mismatch in sync'd repo data. Wolfram implements all of it; this skill routes to the right entry points.

## What this skill provides

- Layer routing: repo mechanics are Wolfram's; the C++23 runtime is the only consumer; the C23 core sees only distilled observations.
- Wolfram mapping: `repo.h`, `sync.h`, `sync_verify.h`, `sync_subscribe.h`, `tid.h` entry points, ownership rules, verification workflow.
- Protocol ground truth: DRISL, CAR v1, MST, commit/signing, data model, test vectors.

## Layer routing

- **C23 core (`include/atperson/`, `src/core/`)** — nothing. No CAR/MST/commit types ever cross into `src/core/`. Observations distilled by the runtime (text, URI, DID, timestamp) are all the graph sees.
- **C++23 runtime (`src/app/`)** — the only atperson layer that could touch repo structures. Uses `wf_sync_*` / `wf_repo_*` / `wf_subscribe_*` through RAII wrappers. Two constraints apply:
  - **Read-only network**: never call the write surfaces (`wf_commit_create`, `wf_repo_create_record`, `wf_repo_apply_writes`, `wf_tid_now` in write flows) from learning pathways.
  - **Durable dedup first (AGENTS.md)**: any future continuous ingestion loop (firehose `seq`, diff `since` rev) must persist a cursor/progress so restarts cannot retrain on the same events. `wf_subscribe_commit.seq`, `wf_repo_diff.since`, and `wf_sync_head.rev` are the cursor candidates.
- **Wolfram (pinned revision)** — all CAR/MST/commit/sync mechanics; never re-implement.

## Defaults

- **DRISL** — canonical DAG-CBOR: bytewise map keys, shortest-form integers, no indefinite-length framing, CIDs as tag 42 + identity multibase. `shared/drisl.md`.
- **CAR v1** — `varint ‖ header-cbor ‖ (varint ‖ 36-byte-CID ‖ bytes)*`; roots declare the commit CID. `shared/car-v1.md`.
- **MST** — SHA-256 key height, fanout 4; node `{l?, e:[{p,k,v,t?}]}`; keys `<collection>/<rkey>`; bounded depth (`wf_mst_node_parse` enforcing `WF_MST_MAX_DEPTH`). `shared/mst.md`.
- **Commit** — `{did, version:3, data, rev, prev, sig}`; signing bytes = DAG-CBOR of the commit minus `sig`. `prev` on genesis is `null` vs omitted — Wolfram models this as `wf_commit.has_prev`. `shared/commit-and-signing.md`.
- **Signature** — raw `r ‖ s` ECDSA, low-S normalized, verified against the signer's `#atproto` key. Wolfram verifies it for you via `wf_repo_verify` / `wf_sync_verify_commit`.
- **Trust ordering** — do not call `wf_mst_find`/`wf_mst_add` on a CAR fetched from the network before integrity verification (`wf_repo_verify` / `wf_repo_diff_verify`); MST functions trust each block's declared CID.
- **Verification options** — `wf_repo_verify_options{expected_did, signing_key, expected_prev}` pins who signed and what precedes.

## Reading guide

For every repo task:

1. Read the relevant `shared/*.md` first (`drisl.md`, `car-v1.md`, `mst.md`, `commit-and-signing.md`, `data-model.md`, `test-vectors.md`).
2. Read `wolfram/README.md` for the API mapping at the pinned revision.
3. For ingestion design, decide the cursor/dedup scheme before wiring any firehose/sync loop (AGENTS.md).

## Common pitfalls

- **`prev: null` vs omitted** — genesis commits sign different bytes depending on encoder. Wolfram exposes `has_prev`; when you compare/verify commit bytes, use Wolfram's parse+verify rather than re-encoding from raw.
- **CID mismatch / unknown codec in CAR** — a CAR block whose declared CID does not hash to its bytes. `wf_repo_verify` catches this; don't skip it on untrusted fetches.
- **MST node not found / missing block** — partial tree. Decide whether the walker should error or use an incomplete result; Wolfram surfaces these as `WF_ERR_*`.
- **prevData vs prev** — firehose `wf_subscribe_commit.prev_data` is the inductive MST root for incremental verification, distinct from commit `prev` (the previous commit CID). Mixing them breaks inverted-diff checks.
- **Write surfaces are read-only policy violations** — never call `wf_repo_create_record`/`wf_repo_apply_writes`/`wf_commit_create`/`wf_tid_now` from learning pathways.
- **No dedup = retraining** — a firehose loop without a persisted `seq`/`rev` cursor violates AGENTS.md.

## Decision rules

- **Ingest via AppView feed (`wf_agent_get_timeline`) or raw repo?** AppView for now; raw repo/firehose ingestion only if observations need record-level provenance, and only after durable dedup exists.
- **Verify before trust?** Yes for anything from `wf_sync_get_repo`/`get_blocks`/`wf_subscribe_decode_frame`: `wf_repo_verify` (or `wf_sync_verify_commit`) before any MST walk.
- **Write records?** Never as a learning side effect. atperson's network path is read-only.

## Verification

Offline (no network) constructs: `wf_car_parse` (well-formed CAR), `wf_cbor_parse`, `wf_mst_node_parse`, `wf_commit_parse`, `wf_tid_encode/decode`, `wf_syntax_record_key_is_valid`, `wf_syntax_tid_is_valid`. End-to-end integrity (`wf_repo_verify`, `wf_sync_verify_commit`) needs a signing key / DID resolution.

Core-only build/tests:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Directory layout

```
atproto-repository/
├── SKILL.md                    # this file — router
├── shared/
│   ├── drisl.md                # canonical DAG-CBOR rules
│   ├── car-v1.md               # CAR v1 byte layout
│   ├── mst.md                  # MST algorithm + invariants
│   ├── commit-and-signing.md   # commit shape, signing bytes, verification
│   ├── data-model.md           # records, NSIDs, TIDs, AT-URIs
│   └── test-vectors.md         # fixtures
└── wolfram/
    └── README.md               # repo/sync mapping at the pinned revision
```

## References

- `shared/drisl.md`, `shared/car-v1.md`, `shared/mst.md`, `shared/commit-and-signing.md`, `shared/data-model.md`, `shared/test-vectors.md`
- `wolfram/README.md`