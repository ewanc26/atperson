# Authoring Guide: atperson agent skills

This `.agents/` directory packages AT Protocol skills for the atperson project.
The goal is that any agent doing atperson work — extracting a learning
observation from the timeline, resolving an author DID, validating a lexicon
record, auditing an OAuth/session path, reasoning about CAR/MST commits — loads
the right specialized knowledge on demand instead of relying on generic
training-time recall.

This file is the authoring guide. It is loaded into the context of any agent
working on atperson, so its conventions apply to every skill in `.agents/`.

## 1. The architecture this guide serves

atperson is a C23/C++23 project (see `AGENTS.md`). Three rules bound every
skill:

- **C23 owns learned state and learning algorithms.** The C core lives in
  `include/atperson/` + `src/core/`. It must stay free of C++ types, STL,
  exceptions, Wolfram dependencies, and network access.
- **C++23 owns application/runtime concerns and AT Protocol integration.**
  `src/app/` owns the runtime. AT Protocol mechanics come from
  `ewanc26/wolfram` (pinned by FetchContent in `CMakeLists.txt`); atperson
  never reimplements a Wolfram API.
- **The network path is read-only.** atperson does not post, reply, like,
  follow, repost, DM, or moderate as a side effect of a learning change. No
  app passwords are logged or persisted; private messages are never ingested
  into the learning graph.

### Layer routing — where does the work land?

A skill must say, for its topic, which layer owns what:

| Layer | Location | What belongs here |
| --- | --- | --- |
| C23 core | `include/atperson/`, `src/core/` | Learned state, graph/neural/persistence logic. Never AT Protocol mechanics. AT Protocol data arrives here only as already-extracted observation payloads (text, source URI, author DID, timestamp). |
| C++23 runtime | `src/app/` | Wolfram-backed AT Protocol integration, RAII wrappers (`AtprotoClient` wraps `wf_agent_handle`), JSON extraction, env/config, scheduling, dedup. |
| Wolfram | `ewanc26/wolfram` public C API | All XRPC, identity, repo/CAR/MST, lexicon validation, OAuth, sync/firehose. Never re-coded locally. |
| Skill knowledge | `shared/`, `references/` | Protocol ground truth: DRISL, CAR v1, DID/handle specs, OAuth internals. |

If a change makes it impossible to run or test the learning graph without the
C++ network runtime, the boundary is wrong (AGENTS.md).

## 2. Skill anatomy

Every skill is a single directory under `skills/`. The directory name is the
skill name.

```
skills/<skill-name>/
├── SKILL.md          ← required. Frontmatter + body. Thin router.
├── shared/           ← protocol ground truth. Language-neutral specs, layouts, test vectors.
├── wolfram/          ← NEW. Mapping onto the pinned Wolfram C API + the atperson layer split.
├── references/       ← optional. Long-form material loaded on demand.
├── scripts/          ← optional. Deterministic helpers (validators, parsers).
└── assets/           ← optional. Templates copied into user outputs. Not loaded into context.
```

`SKILL.md` frontmatter:

| Field | Required | Notes |
| --- | --- | --- |
| `name` | yes | Must match the directory. kebab-case. <30 chars. |
| `description` | yes | The trigger mechanism — see §4. |
| `version` | no | Semver. Conventional but not enforced. |

Body conventions:
- Keep `SKILL.md` under **500 lines**. Spill longer material into `shared/`,
  `wolfram/`, or `references/` and link to it.
- Lead with *When to use* and *What this skill provides*. Put *Procedure*
  next. Everything else is supporting material.
- Reference Wolfram entry-point headers and `wf_*` functions by name (see §5)
  rather than describing protocol mechanics the library already implements.

### Wolfram-mapped skills (the standard shape)

Most AT Protocol topics are neutral at the wire level but concrete at the
atperson layer as: "Wolfram implements X; atperson calls it from the C++23
runtime and feeds a distilled observation to the C core." Use this shape:

```
skills/<skill-name>/
├── SKILL.md          ← thin router. Atperson layer routing + reading guide.
├── shared/           ← language-neutral spec. Kept as protocol ground truth.
└── wolfram/
    └── README.md     ← mapping: entry headers, key wf_* functions, RAII notes,
                        offline-testability, atperson-specific pitfalls.
```

Rules:

- `SKILL.md` is a **router**, not a tutorial. It routes the topic to
  `shared/` (what the protocol actually specifies) and `wolfram/README.md`
  (which pinned-Wolfram API provides it), and states how the C23/C++23 split
  applies. Keep it under ~150 lines.
- `shared/` holds the normative spec and is language-neutral. It may be
  inherited from the upstream ngerakines.me atproto-crates skill set; keep it
  verbatim and never let atperson-specific notes leak in.
- `wolfram/README.md` is atperson-specific. It documents the mapping onto the
  Wolfram API *at the pinned revision* (see CMakeLists `FetchContent`), which
  `wf_*` functions to call, ownership/RAII rules, and what is offline testable.
- If Wolfram has no machinery for the topic, `wolfram/README.md` must say so
  explicitly and state whether atperson has any current use for the topic
  (e.g. attestation: Wolfram only ships the `app.bsky.graph.verification`
  record lexicon; atperson is read-only and has no signing/attestation path).

The canonical example is `skills/atproto-cid/` — mirror its layout.

## 3. Progressive disclosure

Agents pay a context cost for everything they load, so each skill is a funnel:

| Layer | Loaded when | What belongs here |
| --- | --- | --- |
| `description` | always, as metadata | A few sentences. The trigger surface, including `wf_*` API names. Loaded even when the skill is idle. |
| `SKILL.md` | skill is triggered | Layer routing, procedure, decision rules, short examples. ≤500 lines. |
| `shared/`, `wolfram/` | link-followed | Protocol spec + Wolfram API mapping. |
| `references/` | link-followed | Spec excerpts, long examples, reference tables. |
| `scripts/` | invoked deterministically | Executable helpers. Agents call them instead of reasoning. |
| `assets/` | copied into outputs | Boilerplate, templates. Never read into context. |

If a piece of content isn't needed to *trigger* or *execute* the main flow,
push it out of `SKILL.md` and into `shared/`, `wolfram/`, or `references/`.

## 4. Writing effective `description` fields

`description` is the only signal an agent uses to decide whether to load the
skill. Rules:

- **Third person.** "This skill should be used when the user…". Not "I…" and not "you…".
- **Name the concrete terms.** List the AT Protocol vocabulary (see §6) plus
  the Wolfram entry headers / `wf_*` symbols (see §5) this skill handles.
- **Describe trigger situations, not implementation.** "debugging a
  `wf_sync_get_repo` CID mismatch" is a trigger. "Implements a CAR verifier"
  is not something atperson would ever do.
- **Anchor on the architecture.** When the trigger implies network behavior,
  state the read-only constraint and the C23/C++23 split (e.g. "atperson does
  not publish lexicons; this skill is for reasoning about `_lexicon.` TXT
  authority lookups seen during ingestion").
- **Call out adjacent skills.** Say which sibling owns the boundary so
  descriptions don't shadow each other.
- **Err on the side of over-triggering.** An idle skill is worse than a
  slightly over-eager one; the agent can ignore the guidance.

**Before (upstream, polyglot):**
> Triggers on dependency/import names like `atproto-identity` (Rust crate), `@atproto/identity`, `github.com/bluesky-social/indigo/atproto/identity`.

**After (atperson):**
> Triggers on Wolfram symbols like `wf_did_resolve`, `wf_handle_resolve`, `wf_agent_resolve_handle`, `wf_syntax_*_validate`, and phrases like "resolve this author's DID" from the timeline ingestion path in `src/app/`.

## 5. Wolfram: the "already available" implementation surface

Skills must route to Wolfram instead of describing reimplementation. The
pinned revision ships, under `include/wolfram/`:

- **Client/session:** `agent.h` (`wf_agent_new`/`wf_agent_free`/`wf_agent_login`,
  `wf_agent_get_timeline`/`_lex`, `wf_agent_get_record`/`put_record`,
  `wf_agent_resolve_handle`/`verify_handle`, TLS config), `session.h`,
  `auth_client.h`, `xrpc.h`.
- **Identity:** `identity.h` (`wf_did_resolve`, `wf_did_document_parse`,
  `wf_handle_resolve`, `wf_handle_parse_dns_txt`, DID cache),
  `plc.h`, `syntax.h` (`wf_syntax_*_is_valid` / `wf_syntax_*_validate`).
- **Repo/CAR/MST:** `repo.h` umbrella over `repo/cid.h`, `repo/car.h`,
  `repo/cbor.h`, `repo/mst.h`, `repo/commit.h`, `repo/record.h`, `repo/diff.h`.
- **Sync:** `sync.h` (`wf_sync_get_repo`, `wf_sync_verify_diff_car`,
  `wf_sync_get_record`, `wf_sync_get_blocks`), `sync_verify.h`,
  `sync_subscribe.h` (firehose), `jetstream.h`.
- **Lexicon:** `validate.h` (lexicon registry + `wf_validate_record`),
  `lexcall.h` (generic typed decode registry), `lexicon_typed.h`,
  `atproto_lex.h` (generated NSID macros, `wf_lex_json`, `wf_lex_blob`).
- **OAuth:** `oauth.h` umbrella over `oauth/{metadata,pkce,dpop,par,state,callback,flow,verify}.h`.
- **Bluesky domain types:** `feed_typed.h`, `embed_typed.h`, `graph_typed.h`,
  `actor_typed.h`, `repo_typed.h`, etc. plus `lexicons/` catalog of lexicon
  JSON docs.

When in doubt, prefer a Wolfram call over local reimplementation. The one
legitimate "own" area is atperson's learning graph, which consumes only
already-extracted observations — it never wires to XRPC.

## 6. AT Protocol vocabulary checklist

Skill descriptions should draw trigger terms from this vocabulary. If a user
mentions something on this list, *some* skill here should fire.

- Identity: `DID`, `did:plc`, `did:web`, `did:webvh`, handle, handle
  resolution, signing key, rotation key, `_atproto.` TXT, `/.well-known/atproto-did`
- Network: PDS, AppView, relay, feed generator, labeler
- Addressing: `at://` URI, NSID, TID, CID, `$link`, `bafyrei`/`bafkrei`
- Data: record, commit, MST (Merkle Search Tree), CAR, DAG-CBOR, block store
- API surface: XRPC, `com.atproto.*`, `app.bsky.*`, lexicon, query vs procedure
- Streaming: firehose, jetstream, subscribeRepos, event cursor
- Auth: OAuth, DPoP, PAR, scopes, client metadata, service auth (JWT), app passwords
- Moderation: labels, label value definitions, takedowns, thread gate

## 7. Skill index

| Skill | Status | Covers |
| --- | --- | --- |
| `atproto-cid` | landed | CID parse/construct/validate (`$link`, tag 42, `bafyrei`/`bafkrei`), blob vs dag-cbor codec. Wolfram: `repo/cid.h`. |
| `atproto-identity-resolution` | landed | Handle ↔ DID resolution, DNS TXT `_atproto.`, well-known DID, DID doc shape, `handle.invalid`, bidirectional verification. Wolfram: `identity.h`, `plc.h`, `syntax.h`. |
| `atproto-repository` | landed | CAR v1 parse/emit, MST node/tree/diff, DRISL canonical CBOR, commit signing/verification, `at://<did>/<collection>/<rkey>`, TID rkeys, sync/firehose verification. Wolfram: `repo/*.h`, `sync*.h`. |
| `atproto-lexicon` | landed | Lexicon docs, schema validation, backward-compat, XRPC (`query`/`procedure`/`subscription`), `$type` dispatch, `strongRef`, blob refs, `at://` refs in records. Wolfram: `validate.h`, `lexcall.h`, `atproto_lex.h`, `xrpc.h`. |
| `atproto-oauth` | landed | OAuth 2.1 + AT Proto profile (PAR/DPoP/PKCE, client metadata, sessions, refresh races). Wolfram: `oauth/*.h`; atperson currently uses legacy app-password login. |
| `atproto-publish-lexicon` | landed | Publishing lexicons as `com.atproto.lexicon.schema` records and resolving them back by NSID. Knowledge-only for atperson (read-only network). |
| `atproto-attestation` | landed | badge.blue record attestations (inline ECDSA, remote strongRef). Wolfram ships only the verification record lexicon; atperson has no attestation path. |

Bluesky app UI idioms (richtext facets beyond what Wolfram's `richtext.h`
extracts, appview-specific view types) are out of scope for this skill set.

## 8. How to add a new skill

1. `cp templates/SKILL.md.template skills/<your-skill-name>/SKILL.md`.
2. Fill in `name` (must match directory), `description` (read §4 first), and
   the body sections.
3. Put protocol ground truth in `skills/<name>/shared/` and the Wolfram
   mapping in `skills/<name>/wolfram/README.md` (use
   `templates/WOLFRAM-SKILL.md.template`).
4. If you need deterministic helpers, create `scripts/` and call them from the
   *Procedure* section.
5. Verify (§9).

## 9. Local verification loop

Core-only (no Wolfram fetch):

```sh
cmake -S . -B build-core -DATPERSON_BUILD_NETWORK=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Network paths must also build and pass ctest with `ATPERSON_BUILD_NETWORK=ON`.
For skill-trigger tests: start a fresh `opencode` session and prompt using
real user language, e.g. "why does `wf_did_resolve` return not-found for this
author" — confirm the skill loads. Iterating on `description` is the main
loop; treat it like a spec for the trigger, not a marketing blurb.