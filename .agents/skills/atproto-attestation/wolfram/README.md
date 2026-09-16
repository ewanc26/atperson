# wolfram/README.md — atproto-attestation: the Wolfram gap

Verified against the pinned Wolfram revision `9e63f76`.

## Wolfram ships NO attestation machinery

There is no badge.blue `$sig` metadata support, no `signatures[]` create/verify
path, no ECDSA signing or verification, no content-CID-for-signing helper, and
no proof-record tooling. Nothing in `include/wolfram/` implements the
CID-first attestation model described in this skill's `shared/`.

Consequences for atperson:

- **No `wf_*` call exists** to create or verify an attestation. Do not invent
  one; do not route a "verify this record" request to a Wolfram function that
  doesn't exist. Implementation lives only in the external reference crate
  `atproto-attestation` and the shared specs.
- **The C23 core must not acquire crypto.** Even if attestation verification
  were desired, it would be an app-level concern, not learned-state — and
  nothing in atperson currently requires it.
- **Read-only network** means atperson never emits attestation or proof
  records regardless.

## The only related Wolfram surface: `app.bsky.graph.verification`

- `include/wolfram/atproto_lex.h`:
  `#define WF_LEX_APP_BSKY_GRAPH_VERIFICATION_NSID "app.bsky.graph.verification"`
  (kind = record), with a generated typed struct for the record.
- Lexicon doc: `lexicons/app/bsky/graph/verification.json` in the checkout.
  Required fields: `subject` (did), `handle` (handle), `displayName` (string),
  `createdAt` (datetime).

This is a **plain Bluesky record** ("I verify this DID belongs to this handle /
display name"), not the badge.blue attestation signature format. It cannot be
used to create, carry, or check `signatures[]` entries, and it shares no
algorithmic content with them. When parsing such records from the timeline,
treat them as ordinary `app.bsky.*` records (see `atproto-lexicon` /
`feed_typed.h`/`atproto_lex.h` typed decoders), not as attestations.

## Atperson data flow if such records appear

If `subject`/`handle`/`displayName` verification records are ever distilled
into an observation (e.g. "entity X is claimed to be verified by Y"), the
C++23 runtime extracts the plain fields via cJSON/typed decoders into a
`NetworkObservation`; the C core never sees the record shape. Freshness and
trust policy are application concerns, separate from any crypto.

## Gaps

- Attestation create/verify: absent from Wolfram, absent from atperson — keep
  this skill knowledge-only.
- `app.bsky.graph.verification` does not carry the badge.blue meaning; do not
  confuse the two when answering questions.