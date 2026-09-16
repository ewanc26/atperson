# wolfram/README.md — atproto-publish-lexicon mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76`. Entry headers:
`include/wolfram/agent.h` (record read/write), `include/wolfram/atproto_lex.h`
(typed lexicon-schema record), `include/wolfram/validate.h` (offline
validation), `include/wolfram/identity.h` (authority DID lookups).
`include/wolfram/syntax.h` for NSID validation.

## What Wolfram provides

- `wf_agent_get_record(agent, collection, rkey, ...)` — read a
  `com.atproto.lexicon.schema` record for a given NSID rkey. `wf_response*`
  out; free with `wf_response_free`.
- `wf_agent_put_record(agent, collection, rkey, record_json, len)` — the
  publish path. **Off-limits in atperson**: the network path is read-only. If
  a lexicon ever needs publishing, it is an explicit, user-triggered, non-
  learning action performed outside the learning graph — and Wolfram's
  `validate: true`-equivalent (registry validation) must run first.
- Typed schema: `WF_LEX_COM_ATPROTO_LEXICON_SCHEMA_NSID`
  ("com.atproto.lexicon.schema", kind "record") and
  `wf_lex_com_atproto_lexicon_schema_main` for a typed view of the published
  record. `atproto_lex.h` also carries the discovered/resolved lexicon-schema
  record shapes (`wf_lex_com_atproto_lexicon_schema_main` with `schema`/
  `record`/`record_cid`/`uri` fields used by AppView describe functions).
- Offline validation of a candidate document:
  - `wf_lexicon_registry_load(registry, doc_json, len)` — attempt to load the
    document into a `wf_lexicon_registry`; a malformed lexicon doc fails here.
  - `wf_validate_record(registry, "com.atproto.lexicon.schema", doc_json,
    len)` — validate the record against the schema lexicon exactly as a PDS
    would (`id`/`lexicon`/`defs`/`revision` invariants enforced).
  - `wf_lexicon_registry_contains(registry, nsid)` — confirm the document
    actually defines the NSID it claims.
- NSID checks: `wf_syntax_nsid_is_valid` / `wf_syntax_nsid_validate`.

## What Wolfram does NOT provide

- **No `_lexicon.<authority>` TXT lookup.** Wolfram has no DNS helper for the
  lexicon-authority TXT record (its TXT helper `wf_handle_parse_dns_txt`
  targets `_atproto.` handle records). The `_lexicon.` query is a raw DNS TXT
  lookup plus `did=` key parsing — outside Wolfram. For authority checks,
  atperson would resolve DNS itself and then `wf_did_resolve` the resulting
  DID to confirm the publishing actor.
- **No cross-version compatibility checker** (no runtime equivalent of a
  "check_compatibility" tool). Break/non-break reasoning stays in
  `atproto-lexicon/shared/backward-compat.md`; Wolfram only validates a single
  document in isolation.
- **No AppView "describe lexicon by NSID" function.** Wolfram serves
  record-level reads and registry-based validation; it does not encapsulate the
  NSID→authority→DNS→DID→PDS→record resolution chain as one call.

## Ownership / RAII

- `wf_lexicon_registry*` heap-owned → `wf_lexicon_registry_free` (build once at
  startup in the C++23 runtime).
- `wf_validate_result` by-value with owned error list → `wf_validate_result_free(&result)`.
- `wf_response*` heap-owned → `wf_response_free`.

## Layer split (C23 vs C++23)

- **C23 core** — nothing. Published-lexicon records are metadata; any
  observation distilled from them (e.g. "this NSID's authority is known") is a
  plain serialisable value, never a Wolfram type.
- **C++23 runtime** — if the runtime ever resolves a custom lexicon
  (`com.atproto.repo.getRecord` on custom NSIDs), it loads the retrieved
  document into its registry and applies `wf_validate_record`. It never
  publishes (read-only).

## Offline-testable surface

Registry load + `wf_validate_record` against the `com.atproto.lexicon.schema`
schema, `wf_lexicon_registry_contains`, `wf_syntax_nsid_is_valid` — all
network-free; usable in `build-core` tests. `references/publish-checklist.md`
covers the non-code pre-flight steps.

## Gaps

- Publishing is knowledge-only for atperson (read-only network). The full
  publish procedure in `SKILL.md` is for tooling outside atperson.
- Authority validation via `_lexicon.` TXT is not in Wolfram; document that in
  any atperson code that reasons about lexicon authority.