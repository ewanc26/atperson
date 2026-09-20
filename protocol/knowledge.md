# AT Protocol knowledge boundary

This file is a maintained, non-social protocol reference for the evidence
reducer. It is deliberately stored under `./protocol/`; it is not vocabulary,
biography, preference, personality state, or training data.

## Identity and authority

- A handle is mutable human-readable naming metadata. A DID is the stable
  subject identifier. Resolution must retain both values and must never treat
  a handle as a DID.
- A DID document can provide verification methods, rotation history and
  services. The `AtprotoPersonalDataServer` service identifies the PDS for the
  repository; relay, App View, feed-generator and labeler services have
  different roles and are not interchangeable authorities.
- Repository authority belongs to the signed repository and its PDS. App View
  responses are derived views and must be retained as `AppViewDerived`, never
  promoted to repository facts.

## Data model

- An AT-URI identifies a DID, collection NSID and record key. A CID identifies
  content; a TID identifies a repository record key or revision where the
  protocol specifies one. A strong reference is an AT-URI plus CID.
- Records are addressed within collections (NSIDs). Blobs are content objects
  referenced by CID and metadata, not records themselves.
- A repository revision is distinct from a firehose/JetStream sequence. The
  former describes repository history; the latter describes delivery order.
  A filtered stream can have sequence gaps without invalidating the delivered
  commit.
- Explicit deletion is evidence of deletion. Failure to observe a record is
  only `Missing`; it is never inferred as deletion.

## Lexicon and XRPC

- Lexicon schemas define NSIDs and their records, queries, procedures and
  subscriptions. An XRPC query reads; a procedure performs an operation; a
  subscription delivers a stream. Classification comes from the schema or
  endpoint contract, not from an NSID suffix guess.

## Synchronization and verification

- Firehose families such as `#commit`, `#sync`, `#identity` and `#account`
  remain protocol evidence even when they are not social observations.
- A cursor gap creates a bounded repository/CAR resynchronization plan.
  Invalid signatures, malformed identifiers, future revisions and unavailable
  keys remain visible as rejected or unverified evidence.
- Repository commits and signed roots are verified by Wolfram against the
  resolved DID signing key. atperson stores the result, provenance, sequence,
  timestamp and confidence, and never fabricates verification.

## Authorization

OAuth scope, record permissions, repository migration/CAR permissions and
DPoP-bound sessions are separate facts. A DPoP-bound session proves binding of
the token to its key; it does not itself grant record or migration authority.
Learning protocol evidence does not publish, mutate repositories, or activate
outbound actions.

## Source specifications

This reference follows the official specifications and guides:

- https://atproto.com/guides/overview
- https://atproto.com/guides/identity
- https://atproto.com/specs/lexicon
- https://atproto.com/specs/xrpc
- https://atproto.com/specs/sync
- https://atproto.com/guides/data-repos
