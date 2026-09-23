# AT Protocol learning contract

Understanding the AT Protocol is a hard runtime requirement for atperson. The
entity must be able to build and inspect a protocol model from protocol events,
not only learn words from social records.

## Ownership boundaries

Protocol literacy is deliberately separate from social learning:

- `./protocol/` owns protocol vocabulary, structural validation, durable
  protocol evidence, reduction, replay, explanations and capability checks;
- Wolfram owns network transport, XRPC/protocol operations, identity resolution,
  repository retrieval and cryptographic verification;
- the C23 social-content learner must not absorb protocol facts as vocabulary,
  preferences, biography or personality merely because they were observed;
- C++ may orchestrate protocol acquisition and inspection, but it must not
  invent verification or duplicate Wolfram's protocol mechanics.

OAuth session material is runtime credential state, not protocol evidence and
not learned experience. Tokens, authorization codes, refresh state and DPoP
private material must remain outside both the protocol evidence ledger and the
social model. The credential and scope boundary is defined in
[`docs/oauth.md`](oauth.md).

Protocol literacy also never grants network permission. Passing a protocol
capability check does not bypass operator approval, outbound policy, rate
budgets or the Wolfram-backed write path.

## Required concepts

The protocol-learning path must represent, with provenance and confidence:

- identity: DID, handle, DID document, signing key, rotation and PDS service;
- network roles: PDS, relay, App View, feed generator and labeler;
- data model: repository, collection/NSID, record, blob, AT-URI, CID, TID and
  strong references;
- APIs: XRPC NSIDs, query versus procedure, subscription, Lexicon schemas and
  validation;
- synchronization: repository commits, signed roots, CAR export, firehose or
  JetStream `#commit`, `#sync`, `#identity` and `#account` events,
  sequence cursors and bounded backfill/replay;
- authority boundaries: repository data is self-certifying when its repository
  proof verifies, while identity and account hosting status require independent
  resolution/verification and App View results remain derived;
- authorization: OAuth scopes, DPoP-bound sessions and the distinction between
  record permissions and whole-repository CAR migration authority.

These are protocol facts, not seeded personality. They must remain in the
dedicated protocol subsystem so they cannot silently become social preferences,
authored biography or ordinary post vocabulary.

## Evidence and provenance rules

Every protocol observation must retain enough durable provenance to explain why
a fact was accepted, rejected or left unverified. Where available this includes
the source URI or endpoint, evidence/event kind, subject, timestamp, delivery
sequence, repository revision or root CID, verification state and confidence.

The reducer must distinguish at least:

1. a maintained Lexicon/schema or protocol declaration;
2. an observed network or repository event;
3. a local inference produced from already-recorded evidence;
4. an App View or other derived observation; and
5. an unverified claim carried inside record content.

A source saying something is true is not the same as protocol verification.
App View output must never be promoted to repository authority. A repository
commit or CAR becomes authoritative evidence only when the relevant repository
proof has been verified through Wolfram against the resolved DID signing key.

Invalid signatures, malformed CIDs/AT-URIs, impossible/future revisions,
unavailable verification keys and conflicting identity claims must remain
inspectable as rejected or unverified evidence. They must not disappear, be
silently repaired, or enter social learning as facts.

## Replay and reconciliation semantics

The durable protocol evidence ledger is the authority for reconstructing the
protocol model. Replay must be deterministic for a fixed compatible evidence
schema, and duplicate delivery must be idempotent: reconnects, cursor rewinds,
archive replay, repository resync and repeated events must not train or reduce
the same evidence twice.

Delivery cursors and repository revisions are different state. A firehose or
JetStream sequence is delivery order; a repository revision is repository
history. A cursor must never be used to fabricate a repository revision or as
proof that a record was learned or verified.

Filtered JetStream can legitimately skip unrelated global sequence numbers.
Such gaps are reconciliation signals, not permission to discard otherwise valid
selected events. Missing repository revisions remain explicitly unverified until
bounded repository/CAR reconciliation supplies evidence that can be verified.

A detected gap may create a bounded resynchronization plan. It is cleared only
by explicitly committed, verified reconciliation evidence; transport failure,
bound violations, parse failure or signature failure cannot silently advance
authority. Explicit record deletion is evidence of deletion. Merely failing to
observe a record is `Missing`, never an inferred delete.

## Capability gates

Before enabling autonomous protocol-facing behaviour, atperson must pass local
tests demonstrating that it can:

- resolve a handle to a DID and identify the PDS service;
- explain the difference between a DID and a handle;
- parse an AT-URI into DID, collection and record key;
- classify an XRPC call as query, procedure or subscription from its Lexicon;
- track a repository revision and firehose/JetStream sequence independently;
- detect a gap and produce a bounded repository resynchronization plan;
- verify a repository commit against the DID signing key, or explicitly mark
  the evidence unverified;
- distinguish a record delete from a missing observation;
- explain why an App View result is not itself repository authority; and
- distinguish OAuth record scopes from whole-repository migration authority
  without treating authentication as evidence verification.

Capability gates fail closed. Missing, rejected or unverified evidence cannot be
silently upgraded merely to satisfy a gate, and passing a gate never enables an
outbound action by itself.

The maintained acceptance mapping lives in
[`protocol/acceptance.md`](../protocol/acceptance.md). The protocol reference
and implementation boundary live in
[`protocol/README.md`](../protocol/README.md) and
[`protocol/knowledge.md`](../protocol/knowledge.md).

## Inspection surfaces

`atperson protocol status` inspects the durable protocol evidence store and
`atperson protocol explain <subject>` exposes subject-level provenance.
Identity resolution and OAuth planning/metadata inspection are also exposed
through the read-only `protocol` command family.

Network acquisition and cryptographic verification remain Wolfram concerns.
Durable evidence, replay, explanations and capability assessment remain under
`./protocol/`, outside the social model.
