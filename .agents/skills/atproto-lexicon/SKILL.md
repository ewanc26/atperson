---
name: atproto-lexicon
description: This skill should be used when the user is authoring, validating, parsing, or invoking AT Protocol lexicons in atperson — the JSON schema layer that governs record shapes and XRPC methods — via Wolfram in the C++23 runtime. Triggers on phrases like "lexicon", "lexicon doc", "LexiconDoc", "NSID", "defs", "$type", "$type dispatch", "main def", "open union", "closed union", "knownValues", "enum", "strongRef", "blob ref", "cid-link", "record-key", "rkey", "tid", "at-uri", "at://<did>/<collection>/<rkey>", "record validation", "validate record", "query", "procedure", "subscription", "XRPC", "XRPC method", "invoke XRPC", "xrpc call", "params", "input.schema", "output.schema", "subscription frame", "MessageFrame", "ErrorFrame", "firehose consumer", "Jetstream", "backward-compat", "breaking change", "add optional field", "closed union evolution", "InvalidRequest", "XRPCError", "RateLimitExceeded". Also triggers on Wolfram symbols and entry headers: `wf_validate_record`, `wf_validate_value`, `wf_validate_result`, `wf_lexicon_registry_new`/`_load`/`_load_dir`/`_contains`/`_free`, `wf_lex_decode_output`, `wf_lex_call_output`, `wf_lex_entry`, `wf_xrpc_query`, `wf_xrpc_procedure`, `wf_response`, `WF_LEX_*_NSID`, `WF_LEX_*_KIND`, `wf_lex_json`, `wf_lex_blob`, `wf_lex_cid_link`, `include/wolfram/validate.h`, `include/wolfram/lexcall.h`, `include/wolfram/atproto_lex.h`, `include/wolfram/xrpc.h`, `wf_agent_get_timeline_lex`. Use this skill to validate a record before shaping it into an observation, convert feed JSON into a typed post via Wolfram's lexicon registry, call any `com.atproto.*`/`app.bsky.*` XRPC query/procedure, consume a subscription, or plan a backward-compatible lexicon change. Covers lexicon document structure, NSID grammar, AT-URI shape in records, `$type` dispatch, strongRef vs blob refs, XRPC HTTP/WebSocket wire format, validation semantics, and the backward-compat change matrix. In atperson lexicon mechanics are owned by Wolfram (`wf_lexicon_registry_*`, `wf_validate_*`); record extraction may run in the C++23 runtime, and the C23 learning graph receives only distilled observations, never raw records or Wolfram types. Does NOT cover CID parsing/construction (see `atproto-cid`), DID resolution / handle lookup (see `atproto-identity-resolution`), CAR/MST/commit signing (see `atproto-repository`), OAuth token flows (see `atproto-oauth`), or publish/resolve of lexicon records on the network (see `atproto-publish-lexicon`). Bluesky-domain record idioms (`app.bsky.*` facets, richtext, embeds, threadgates, labels) are out of scope.
version: 0.1.0
---

# AT Protocol Lexicon + XRPC + Records in atperson

An AT Protocol **lexicon** is a JSON document that declares the shape of one record type or one XRPC method. Every record's `$type`, every XRPC path, and every `ref` between them ties back to an NSID. The lexicon is the protocol's type system; validation against it is the contract between clients, servers, and the repository layer.

## When to use

Validating a record pulled from the feed before it becomes a learning observation, reasoning about a record's `$type`/strongRef/blob shape, parsing XRPC errors from `wf_agent_get_timeline`/`wf_xrpc_query`, or deciding whether a lexicon change is backward-compatible.

## What this skill provides

- Layer routing: lexicon validation/parsing is Wolfram's; runtime record→observation extraction is the C++23 layer's; the C23 core sees only distilled output.
- Wolfram mapping: `validate.h`, `lexcall.h`, `atproto_lex.h`, `xrpc.h` entry points, validation-result ownership.
- Protocol ground truth: lexicon docs, NSID/AT-URI grammar, record model, XRPC wire format, backward-compat matrix, fixtures.

## Layer routing

- **C23 core (`include/atperson/`, `src/core/`)** — nothing. Raw records, lexicon types, and XRPC payloads never enter `src/core/`. The runtime converts records to `NetworkObservation` primitives.
- **C++23 runtime (`src/app/`)** — the only layer that parses/validates records. Two routes:
  - **Raw JSON + cJSON extraction** — what `AtprotoClient::fetch_timeline` does today (`wf_agent_get_timeline` raw JSON, then cJSON reads).
  - **Typed decode** — `wf_agent_get_timeline_lex` + `wf_lex_decode_output` / `WF_LEX_*` generated decoders for structured access. Prefer typed decode when adding record fields to observations.
  - Validating a record against a lexicon: load the catalog once (`wf_lexicon_registry_new` + `_load_dir`), then `wf_validate_record` per record.
- **Wolfram (pinned revision)** — every lexicon/XRP/record mechanic. Never write a lexicon validator (AGENTS.md).

## Defaults

- **Lexicon doc** — `{lexicon: 1, id: <nsid>, revision?, defs:{...}}`; primary types (`record`/`query`/`procedure`/`subscription`) under `main`. `shared/lexicon-spec.md`.
- **NSID** — reversed-DNS authority + name segment, ASCII, ≤317 bytes, name segment allows no hyphens. `shared/nsid.md`.
- **`$type`** — required on every record; bare NSID implies `#main`; union dispatch. Missing `$type` = invalid.
- **AT-URIs in records** — DIDs strongly preferred over handles; query strings forbidden. `shared/at-uri.md`.
- **strongRef** — `{uri, cid}` with `cid` a **plain string**, not a `$link`. Common bug source.
- **Blob refs (modern)** — `{$type:"blob", ref:{$link:<cid>}, mimeType, size}`. Legacy `{cid, mimeType}` only with an explicit lenient opt-in (Wolfram's `wf_validate_*` does not infer it).
- **XRPC wire** — `/xrpc/<nsid>`, GET for queries, POST for procedures, WS for subscriptions. Errors `{error, message}`. `shared/xrpc-wire.md`.
- **Validation** — strict by default, lenient-only options for legacy shapes. `wf_validate_record` returns a linked list of `{path, message}` errors.

## Reading guide

For every lexicon / records / XRPC task:

1. Read the relevant `shared/*.md` first (`lexicon-spec.md`, `record-model.md`, `at-uri.md`, `nsid.md`, `xrpc-wire.md`, `backward-compat.md`, `test-vectors.md`).
2. Read `wolfram/README.md` for the API mapping at the pinned revision.
3. For record→observation extraction, read `src/app/atproto_client.cpp` to extend the existing pattern instead of adding a new path.

## Common pitfalls

- **strongRef `cid` is a string, not a `$link`** — emit/`parse` differently; see `shared/record-model.md §3`.
- **Blob shape divergence** — modern `{$type:"blob", ref:{$link}, …}` is the only shape for new writes; legacy `{cid, mimeType}` is not automatically accepted.
- **`$type` missing on a record** — nothing to dispatch on; strict validators reject.
- **Closed vs open unions** — adding a ref to a closed union is breaking; default to open.
- **Freeing validate results** — `wf_validate_result_free(result)`; `success` flags 1/0 with `errors` a linked list.
- **Loading the registry repeatedly** — build `wf_lexicon_registry` once at startup (`_load_dir` over Wolfram's `lexicons/` catalog), not per record.
- **KEEP THE C23 BOUNDARY** — distilled observations only; raw `wf_lex_*`/record structs stay in `src/app/`.

## Decision rules

- **Raw JSON vs typed decode?** Typed (`wf_agent_get_timeline_lex`/`wf_lex_decode_output`) when the observation needs structured record fields; raw JSON + cJSON when it only needs text/URI/DID.
- **Validate every record before learning?** Yes once validation is wired — a malformed record would otherwise poison the graph with a garbage observation. Reject-and-log, don't coerce.
- **Backward-compat check before changing a lexicon?** Yes — `shared/backward-compat.md` matrix; this matters if atperson ever consumes a bespoke lexicon via `wf_lexicon_registry_load`.

## Verification

Offline (no network):

```
# validate a candidate record JSON against a loaded registry
wf_lexicon_registry_new(); wf_lexicon_registry_load_dir(registry, "lexicons");
wf_validate_record(registry, nsid, record_json, len);   // freed with wf_validate_result_free
# typed decode of a known output shape (no network needed for pure parse)
wf_lex_decode_output(nsid, json, len, &out);             // freed with wf_lex_output_free
```

Core-only build/tests:

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

## Directory layout

```
atproto-lexicon/
├── SKILL.md                    # this file — router
├── shared/
│   ├── lexicon-spec.md         # lexicon doc structure + validation rules
│   ├── nsid.md                 # NSID grammar and reserved prefixes
│   ├── at-uri.md               # AT-URIs in records and refs
│   ├── record-model.md         # $type, strongRef, blob refs
│   ├── xrpc-wire.md            # HTTP + WebSocket wire format
│   ├── backward-compat.md      # breaking-vs-non-breaking matrix
│   └── test-vectors.md         # canonical fixtures
└── wolfram/
    └── README.md               # validate/lexcall/atproto_lex mapping
```

## References

- `shared/lexicon-spec.md`, `shared/nsid.md`, `shared/at-uri.md`, `shared/record-model.md`, `shared/xrpc-wire.md`, `shared/backward-compat.md`, `shared/test-vectors.md`
- `wolfram/README.md`