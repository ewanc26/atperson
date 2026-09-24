# atperson lexicon catalog

AT Protocol lexicon documents for the records ATperson publishes under its
own authority. AT Proto is **just JSON**: a lexicon is a small JSON schema
document describing one NSID's record shape or XRPC surface, and every record
written to a PDS is a single tagged JSON object validated against that shape.

The entity lives under the `click.croft.*` namespace (authority `croft.click`),
kept separate from the `com.atproto.*` and `app.bsky.*` namespaces it only
consumes. Wolfram ships the lexicon *mechanic* (registry, validation, typed
decode); this catalog is atperson's *own* schema surface.

## Layout

Each lexicon is one JSON document, one NSID, path-mirroring its NSID:

```
lexicons/<nsid segments>/<name>.json      # e.g. click.croft.thought → lexicons/click/croft/thought.json
```

Every document follows the top-level shape `{lexicon: 1, id, revision?, description?, defs}`.
Details and validation rules: `.agents/skills/atproto-lexicon/shared/lexicon-spec.md`.

## Records

| NSID | Def | Record key | Purpose |
| --- | --- | --- | --- |
| `click.croft.thought` | `main` (record) | `tid` | A self-authored internal note — reflection, consolidation summary, or movement trigger notice. Mirrors the durable thought store (`src/app/thought/store.cpp`, #151) so the local record publishes in full. |
| `click.croft.atperson.metric` | `main` (record) | `tid` | A schema-versioned longitudinal self-evaluation snapshot — cumulative action execution, terminal intent success, reply ratio, valence drift, familiarity growth and a bounded per-period trace. Mirrors the durable metric store (`src/app/selfeval/store.cpp`, #153) so the local record publishes in full. |

## Rules

- **One record, one JSON object.** A thought is a single DAG-CBOR/JSON record,
  which is why the local store is one file per record (`<data>/thoughts/<id>.json`).
- **Record key = creation order.** `tid` keys are lexicographically ordered by
  creation, so `com.atproto.repo.listRecords` in ascending order replays the
  entity's thoughts in the order they were written.
- **Backward-compatible changes only.** Add optional fields, new `knownValues`
  entries (never `enum` members), or extend an open union. Removing fields or
  tightening constraints is a breaking change. See
  `.agents/skills/atproto-lexicon/shared/backward-compat.md`.
- **Validated against the catalog before publish** with Wolfram's
  `wf_lexicon_registry_load_dir(registry, "lexicons")` +
  `wf_validate_record` — a malformed record must never reach the PDS.
- **No outbound dictionary/behaviour schema.** The catalog types only what the
  entity writes as its own durable experience; it is not a transport for
  operator commands or learned scoring state.