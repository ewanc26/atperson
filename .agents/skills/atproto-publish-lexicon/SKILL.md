---
name: atproto-publish-lexicon
description: This skill should be used when the user is publishing an AT Protocol lexicon as a `com.atproto.lexicon.schema` record on the network, or resolving such a record back to a lexicon document. Triggers on phrases like "publish my lexicon", "publish lexicon to PDS", "lexicon publication", "lexicon registry", "schema registry", "who owns this NSID", "NSID authority", "`_lexicon.` TXT record", "lexicon DNS record", "lexicon resolution", "resolve an NSID to a lexicon", "describe_lexicon", "`com.atproto.lexicon.schema`", "putRecord lexicon", "createRecord lexicon", "lexicon revision", "lexicon versioning", "publish NSID", "how do clients fetch my lexicon", "rkey is the NSID", "InvalidRecord when publishing a lexicon". Also triggers on the Wolfram symbols `WF_LEX_COM_ATPROTO_LEXICON_SCHEMA_NSID`, `wf_lex_com_atproto_lexicon_schema_main`, `wf_agent_put_record`, `wf_agent_get_record`, `wf_validate_record` against `com.atproto.lexicon.schema`, and on the `rkey = <full NSID>` convention and the authority-to-DID binding that gates resolution at consumer time. atperson itself does NOT publish lexicons — the network path is read-only (AGENTS.md). In atperson this skill is knowledge-only: it exists for reasoning about lexicon authority/resolution during ingestion, and for validating custom lexicon documents offline when they are loaded into the C++23 runtime's lexicon registry. Does NOT cover authoring the lexicon JSON document, catalog loading, codegen, or client-side record/XRPC validation (see `atproto-lexicon`); DID/handle resolution internals or the `_atproto.` TXT record (see `atproto-identity-resolution`); CAR/MST/commit signing (see `atproto-repository`); CID computation internals (see `atproto-cid`); or Bluesky-domain (`app.bsky.*`) lexicons (out of scope for this skill set).
version: 0.1.0
---

# AT Protocol Lexicon Publication

A lexicon isn't published until it's a record. This skill covers the protocol-level workflow that takes a validated lexicon JSON document, wraps it as a `com.atproto.lexicon.schema` record, writes it to the authoring DID's repo, and makes it resolvable by NSID. The inverse — consumers resolving an NSID back to a lexicon — is the other half.

Authoring the JSON document itself is the sibling skill `atproto-lexicon`'s job. This skill starts the moment you're ready to put that document on the network — or to understand why a fetched `com.atproto.lexicon.schema` record does or doesn't resolve.

## When to use

- The user is publishing a new lexicon or a revision to an existing one (`putRecord` on `com.atproto.lexicon.schema`).
- The user is debugging why a published lexicon doesn't resolve — authority mismatch, missing `_lexicon.` TXT, wrong rkey.
- The user is consuming a lexicon by NSID at runtime and wants the resolution chain (NSID → authority → DNS → DID → PDS → record).
- The user is planning a breaking change and needs to know how `revision` and the authority model interact.
- The user loads a custom lexicon document into the atperson registry and needs to know how to validate it the way the network would.

## atperson positioning

atperson's network path is **read-only** — it never publishes records. This skill therefore has two atperson uses:

1. **Knowledge**: audio/authority model for reasoning about `com.atproto.lexicon.schema` records seen during timeline/record ingestion, and for reading tooling that publishes lexicons.
2. **Offline validation**: checking a custom lexicon document the way a PDS would, without publishing — load it into a `wf_lexicon_registry` and validate the record against the `com.atproto.lexicon.schema` lexicon.

If the user asks to *write* a lexicon record from atperson, stop and flag the AGENTS.md constraint rather than wiring a `wf_agent_put_record` call into a learning flow.

## Procedure (for a real publish, outside atperson)

Follow these steps in order. Skipping authority or compatibility checks is how lexicons ship broken.

1. **Form the record.** A `com.atproto.lexicon.schema` record is the lexicon document with a `$type` stamped on top. Top-level fields:
   - `$type`: `com.atproto.lexicon.schema` (required)
   - `lexicon`: `1` — the lexicon *language* version, fixed at 1 today (not a semver of your doc)
   - `id`: the NSID — **MUST equal the rkey you'll publish under**
   - `revision`: optional integer; omit for the first publish, bump monotonically thereafter
   - `description`: optional top-level string
   - `defs`: the map of definition names to def objects (`main`, plus any secondary defs)

   See `references/record-shape.md` for a field-by-field breakdown.

2. **Validate the lexicon body.** A malformed `defs` map, an unresolvable internal `#ref`, or a missing `main` when the NSID names a `record`/`query`/`procedure`/`subscription` will be rejected. Fix every error before touching the network.

3. **Check authority.** Derive the authority domain by reversing the first NSID segments *except* the final name segment:
   - `com.example.foo.getBar` → authority = `example.com`
   - `app.bsky.feed.post` → authority = `bsky.app`

   Then query the DNS `_lexicon.<authority>` TXT record — it contains `did=did:plc:...` (any DID method). That DID is the only one whose publications under this NSID prefix will be honored by consumers. The DID publishing *from* must match; if it doesn't, the publish will succeed on the PDS but silently no-op at resolution time. See `references/authority-and-ownership.md` for the squatting model and edge cases.

4. **Check for a prior version.** Fetch the current on-network record, if any (`com.atproto.repo.getRecord` with `rkey = <full NSID>`). If one exists: run a compatibility check against it, bump `revision` to `old.revision + 1` (or `1` if absent), and **refuse to publish breaking changes silently** — either mint a new NSID (`com.example.foo.getBarV2`) or get explicit acknowledgement. The break/non-break matrix lives in `atproto-lexicon`'s `shared/backward-compat.md`; revision-specific rules in `references/backward-compat-revisions.md`.

5. **Compute the canonical CID.** Round-trip the record through DRISL canonical CBOR and confirm your encoder's CID matches the canonical computation (see `atproto-cid`/`atproto-repository`). A mismatched CID means consumers can't verify the record against any reference you hand them.

6. **Publish.** `com.atproto.repo.putRecord`:
   ```
   collection: com.atproto.lexicon.schema
   rkey:       <the full NSID, verbatim — e.g. com.example.foo.getBar>
   record:     <the doc from step 1>
   validate:   true
   ```
   Use `putRecord` (idempotent) rather than `createRecord` for updates. The PDS re-validates against the `com.atproto.lexicon.schema` lexicon; the most common reject is `id != rkey`.

7. **Verify resolution end-to-end.** Re-fetch via `getRecord` and compare the returned CID to step 5's. A resolution failure with a working `getRecord` usually means the `_lexicon.` TXT is missing or points at the wrong DID.

## Resolution (consumer side)

1. Reverse-DNS the NSID → authority domain (same rule as step 3).
2. Resolve authority → DID via the `_lexicon.<authority>` TXT `did=` key.
3. Resolve DID → PDS via standard DID-doc resolution — defer to `atproto-identity-resolution`.
4. Fetch with `com.atproto.repo.getRecord`: `repo = <did>`, `collection = com.atproto.lexicon.schema`, `rkey = <full NSID>`.
5. Use the record as a lexicon document — hand to the catalog (`atproto-lexicon` covers catalog loading).

## Directory layout

```
atproto-publish-lexicon/
├── SKILL.md                    # this file — router, knowledge-only for atperson
├── wolfram/
│   └── README.md               # what Wolfram exposes vs the read-only boundary
└── references/
    ├── record-shape.md         # com.atproto.lexicon.schema field-by-field
    ├── resolution-flow.md      # NSID → authority → DNS → DID → PDS → record
    ├── authority-and-ownership.md
    ├── backward-compat-revisions.md
    └── publish-checklist.md
```

## References

- `references/record-shape.md`, `references/resolution-flow.md`, `references/authority-and-ownership.md`, `references/backward-compat-revisions.md`, `references/publish-checklist.md`
- `wolfram/README.md`

External specs:

- <https://atproto.com/specs/lexicon> — lexicon document + resolution.
- <https://atproto.com/specs/nsid> — NSID grammar and authority rules.
- `lexicons/com/atproto/lexicon/schema.json` in `bluesky-social/atproto` — the record lexicon this skill publishes against.

Adjacent skills:

- `atproto-lexicon` — authoring the JSON, catalog loading, codegen, client-side validation, XRPC invocation.
- `atproto-identity-resolution` — DID/handle resolution, `_atproto.` TXT, DID-doc shape.
- `atproto-repository` — CAR/MST/commit signing, DRISL canonical CBOR.
- `atproto-cid` — CID parsing/construction, tag 42.