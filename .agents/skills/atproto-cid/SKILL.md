---
name: atproto-cid
description: This skill should be used when the user is working with AT Protocol or DASL CIDs (Content Identifiers) in atperson — parsing, constructing, validating, verifying, comparing, or debugging them inside the C++23 runtime via Wolfram. Triggers on phrases like "parse a CID", "compute a CID for this record", "verify a blob CID", "why is this CID invalid", "CID mismatch", "tag 42 in DAG-CBOR", "identity multibase prefix", "the $link format", "base32lower", "bafyrei / bafkrei prefix", "dag-cbor codec", "BLAKE3 CID", "BDASL". Also triggers on Wolfram symbols `wf_cid_from_string`, `wf_cid_to_string`, `wf_cid_of_block`, `wf_cid_of_bytes`, `wf_cid_hasher_*`, `cid_equal`, and the entry header `include/wolfram/repo/cid.h` at the pinned revision 9e63f76. Covers the strict DASL CID profile (CIDv1 only, codec raw 0x55 or dag-cbor 0x71, hash SHA-256 0x12, 32-byte digest, base32lower string form with 'b' prefix, 36-byte binary form), the BDASL extension permitting BLAKE3 (0x1e), the DAG-CBOR wire form (CBOR tag 42 plus identity multibase 0x00 prefix), and the AT Protocol JSON `{"$link": "..."}` form. CIDs are owned by Wolfram in atperson — never re-implement parsing/construction. Does NOT cover general IPFS / multiformats CID questions (DASL is a strict subset — reject anything outside the allowed constants); for MST traversal, CAR inspection, or DAG-CBOR canonicalization beyond the CID tag see `atproto-repository`.
version: 0.2.0
---

# AT Protocol / DASL CIDs in atperson

Content Identifiers (CIDs) are the content-addressed hashes that bind every record, blob, and commit in AT Protocol to its exact bytes. In atperson they surface as bare strings in timeline JSON (`$link`, embedded record refs) and as `wf_cid` values inside Wolfram repo/CAR structures. This skill routes protocol ground truth to `shared/` and the atperson implementation surface to `wolfram/README.md`.

## When to use

Debugging a CID string seen in ingested timeline JSON, checking whether a blob reference CID corresponds to bytes a PDS returned, comparing record refs for equality, or reasoning about the DAG-CBOR `$link` / tag-42 wire form of AT Protocol records.

## What this skill provides

- Routing: which atperson layer ever touches CIDs (C++23 runtime, via Wolfram — never the C23 core directly), and which Wolfram functions provide each operation.
- Wolfram mapping: `repo/cid.h` entry points, ownership rules, offline-testable surface.
- Protocol ground truth: DASL subset rules, binary layout, `$link` JSON form, test vectors.

## Layer routing

- **C23 core (`include/atperson/`, `src/core/`)** — nothing. The learning graph never sees CIDs. If an observation needs to identify a record, pass the opaque string (source URI / rkey) distilled by the runtime; CIDs never cross into `src/core/`.
- **C++23 runtime (`src/app/`)** — the only atperson layer that touches CIDs. Let Wolfram parse, compute, and compare. `AtprotoClient` returns distilled `NetworkObservation`s (`text`, `source_uri`, `author_did`, `created_at`); CID handling, when added, goes through `wf_cid_*` and stays in `src/app/`.
- **Wolfram (pinned revision `include/wolfram/repo/cid.h`)** — all parse/construct/validate mechanics. Never hand-roll a multibase or SHA-256 pipeline locally (AGENTS.md: do not duplicate Wolfram APIs).

## Defaults

- **DASL subset** — CIDv1 only, codec `raw` (`0x55`) or `dag-cbor` (`0x71`), SHA-256 (`0x12`), 32-byte digest, base32lower string form with leading `b`, 36-byte binary form. BDASL adds BLAKE3 (`0x1e`) for large-file blobs. Anything else is rejected. Full rules: `shared/spec.md`. Bytes: `shared/binary-layout.md`. Fixtures: `shared/test-vectors.md`.
- **String prefix sniff test** — `bafyrei…` = dag-cbor/SHA-256 record or MST node; `bafkrei…` = raw/SHA-256 blob.
- **`$link` form** — in AT Protocol JSON, CIDs are `{"$link": "..."}`, never a bare string.
- **Validation vs verification** — parsing into the DASL subset is validation; re-hashing content and comparing bytes is verification, a separate step Wolfram's `wf_cid_of_block` / `wf_cid_of_bytes` perform for you.
- **Equality** — compare 36-byte binary forms, not strings.

## Reading guide

For every CID task:

1. Read `shared/spec.md` first. It defines the rules Wolfram enforces and the semantics your code reason about.
2. Read `wolfram/README.md` for the API mapping at the pinned revision.
3. For blob-reference extraction from feed JSON (when wired), read the matching `src/app/` integration (`atproto_client.cpp`, cJSON field access).

Decide layer placement before writing code: if the change would put a CID in the learning graph, or put Wolfram types in `src/core/`, the boundary is wrong.

## Common pitfalls

- **Missing `0x00` identity multibase prefix** inside DAG-CBOR tag-42 wrapping — Wolfram handles this; never re-encode tag 42 by hand.
- **CIDv0 `Qm…` / base58btc `z…` / base64 `m…`** — multiformats-valid but not DASL. `wf_cid_from_string` rejects non-CIDv1 forms; treat rejection as correct behavior.
- **Confusing dag-pb (`0x70`) with dag-cbor (`0x71`)** — one codec byte apart; dag-pb is IPFS-only.
- **Conflating validation with verification** — Wolfram gives you both (`wf_cid_from_string` vs `wf_cid_of_block`/`wf_cid_of_bytes`); use the right one.
- **Comparing strings instead of bytes** — use `cid_equal(const wf_cid*, const wf_cid*)` (note the unprefixed name in `repo/cid.h`).
- **Rust/TypeScript/Go idioms** — not part of atperson; ignore any upstream guidance mentioning them.

## Decision rules

- **DASL vs BDASL?** SHA-256 everywhere in AT Protocol repo graph; BLAKE3 only where the platform opts in for large-file content. Wolfram's `wf_cid_of_block`/`wf_cid_of_bytes` compute the SHA-256 forms atperson needs.
- **`raw` vs `dag-cbor`?** `dag-cbor` for structured records, `raw` for opaque blobs. `wf_cid_of_block` = dag-cbor, `wf_cid_of_bytes` = raw (blob `ref`).
- **Where do CIDs live in atperson?** Only in the C++23 runtime. Strings in logs/JSON, `wf_cid` at Wolfram boundaries. Never in the C23 learning graph.

## Verification

Offline, no network runtime required:

```
# parse an ingested $link value — returns WF_OK / WF_ERR_INVALID_ARG
wf_cid_from_string(str, &out)

# equality of two refs
cid_equal(&a, &b)
```

Core-only build/tests (AGENTS.md):

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Directory layout

```
atproto-cid/
├── SKILL.md                    # this file — router
├── shared/
│   ├── spec.md                 # normative DASL rules
│   ├── binary-layout.md        # byte-level diagrams
│   └── test-vectors.md         # fixtures
└── wolfram/
    └── README.md               # repo/cid.h mapping at the pinned revision
```

## References

- `shared/spec.md`, `shared/binary-layout.md`, `shared/test-vectors.md`
- `wolfram/README.md`