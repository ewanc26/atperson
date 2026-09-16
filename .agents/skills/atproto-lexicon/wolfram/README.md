# wolfram/README.md — atproto-lexicon mapping onto the pinned Wolfram API

Verified against the pinned Wolfram revision `9e63f76`. Headers:
`include/wolfram/validate.h`, `include/wolfram/lexcall.h`,
`include/wolfram/atproto_lex.h`, `include/wolfram/xrpc.h`,
`include/wolfram/agent.h`, plus the `lexicons/` catalog of lexicon JSON docs
in the checkout.

## Entry headers

- `validate.h` — lexicon registry load + record/value validation. The workhorse for atperson.
- `lexcall.h` — generic NSID-keyed typed decode registry (`wf_lex_decode_output`/`wf_lex_call_output`/`wf_lex_output_free`).
- `atproto_lex.h` — generated typed lexicon structs + `WF_LEX_<NSID>_NSID` / `WF_LEX_<NSID>_KIND` macros + shared `wf_lex_json`/`wf_lex_bytes`/`wf_lex_cid_link`/`wf_lex_blob` field types (used by the typed decryptors scattered across `*_typed.h`).
- `xrpc.h` — raw transport: `wf_xrpc_client_new(service_base_url)`, `wf_xrpc_query`, `wf_xrpc_query_params`, `wf_xrpc_procedure`, `wf_response` + `wf_response_free`, `wf_xrpc_error`.
- `agent.h` — session-aware: `wf_agent_get_timeline`/`wf_agent_get_timeline_lex`, `wf_agent_get_record`/`put_record`, feed queries, typed `_lex` variants.
- `lexicons/` — the lexicon JSON catalog Wolfram ships (and `wf_lexicon_registry_load_dir` consumes).

## Key functions

- `wf_lexicon_registry *wf_lexicon_registry_new(void)` — empty registry. Freed with `wf_lexicon_registry_free`.
- `wf_status wf_lexicon_registry_load(registry, json, len)` / `wf_lexicon_registry_load_dir(registry, dir)` — load one lexicon doc or a directory of docs (`lexicons/`). `wf_lexicon_registry_contains(registry, id)` checks presence.
- `wf_validate_result wf_validate_record(const wf_lexicon_registry*, const char *lexicon_id, const char *record_json, size_t len)` — strict record validation. Returns `{int success; wf_validate_error *errors}` where `wf_validate_error{path, message, next}` is a linked list; free with `wf_validate_result_free`. Path strings look like `"record/text"`.
- `wf_validate_result wf_validate_value(registry, lexicon_id, def_id, json, len)` — validate against a named def (e.g. a union or input/output schema) rather than the `main` record def.
- `wf_status wf_lex_decode_output(const char *nsid, const char *json, size_t len, void **out)` — typed decode through the registry; `wf_lex_call_output` is the XRPC-output variant; `wf_lex_output_free(nsid, out)` frees. Typecodes follow `WF_LEX_<NSID>_KIND` (record/query/procedure/subscription/definition).
- `wf_xrpc_query`/`wf_xrpc_query_params`/`wf_xrpc_procedure` — raw XRPC with a `wf_response *out`; raw body is JSON accessible through `wf_response`; `wf_xrpc_error(resp, &err, &msg)` surfaces `{error, message}`.
- `wf_agent_get_timeline(agent, limit, cursor, wf_response *out)` / `wf_agent_get_timeline_lex(...)` — the AppView-level feed used by atperson ingestion; `_lex` yields typed feed items.

## Ownership / RAII

- Registry is heap-owned → `wf_lexicon_registry_free`. Build once at startup.
- `wf_validate_result` is by-value with owned linked list → `wf_validate_result_free(&result)`.
- Typed decode outputs are heap-owned → `wf_lex_output_free(nsid, out)`.
- `wf_response` heap-owned → `wf_response_free`.
- In `src/app/`, wrap registry + response in RAII shells; never leak Wolfram types into a `NetworkObservation`.

## Layer split (C23 vs C++23)

- **C23 core** — sees only distilled observations. No lexicon types, no records, no `wf_validate_result`.
- **C++23 runtime** — loads the registry at startup, validates records (reject-and-log on failure), extracts `text`/URI/DID/timestamp via cJSON or typed decoders, builds `NetworkObservation`s. Currently `AtprotoClient::fetch_timeline` uses raw JSON + cJSON; typed decode (`wf_agent_get_timeline_lex` + `wf_lex_decode_output`) is the upgrade path for structured record fields.

## Offline-testable surface

Registry load, `wf_validate_record`/`wf_validate_value`, `wf_lex_decode_output` (pure parse), `wf_lexicon_registry_contains`, `wf_syntax_nsid_is_valid` — all network-free; usable in `build-core` tests against the `lexicons/` catalog and `shared/test-vectors.md` fixtures.

## Gaps

- Wolfram ships validated generated types for the standard lexicons; there is no runtime "author a new lexicon" path in-Wolfram (that is `atproto-publish-lexicon`/knowledge territory).
- Subscription/firehose frame *framing* lives in `sync_subscribe.h`; per-kind typed frame decoding is at the `wf_subscribe_event` level (see `atproto-repository`).
- `wf_xrpc_procedure`/upload are write-path XRPC — read-only policy means atperson should not call them from learning flows.