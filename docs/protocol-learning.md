# AT Protocol learning contract

Understanding the AT Protocol is a hard runtime requirement for atperson. The
entity must be able to build and inspect a protocol model from protocol events,
not only learn words from social records.

## Required concepts

The protocol-learning path must represent, with provenance and confidence:

- identity: DID, handle, DID document, signing key, rotation and PDS service;
- network roles: PDS, relay, App View, feed generator and labeler;
- data model: repository, collection/NSID, record, blob, AT-URI, CID, TID and
  strong references;
- APIs: XRPC NSIDs, query versus procedure, Lexicon schemas and validation;
- synchronization: repository commits, signed roots, CAR export, firehose
  `#commit`, `#sync`, `#identity` and `#account` events, sequence cursors and
  backfill/replay;
- authority boundaries: repository data is self-certifying, while identity
  and account hosting status require independent resolution/verification;
- authorization: OAuth scopes, DPoP-bound sessions and the distinction between
  record permissions and whole-repository CAR migration authority.

These are protocol facts, not seeded personality. They must live in a separate
protocol-knowledge namespace or ledger so they cannot silently become social
preferences, authored biography or ordinary post vocabulary.

## Learning and verification rules

Every learned protocol fact must retain its source URI or endpoint, observed
event type, timestamp/sequence where available, and verification status. The
entity must distinguish:

1. a Lexicon/schema declaration;
2. an observed network event;
3. a local inference; and
4. an unverified claim from record content.

Protocol knowledge must be replayable from its durable evidence. A reconnect,
cursor rewind, duplicate event or repository re-sync must not train a fact
twice. Invalid signatures, malformed CIDs/AT-URIs, future revisions and
untrusted identity claims must remain visible as rejected evidence rather than
being learned as facts.

## Capability gates

Before enabling autonomous protocol-facing behaviour, atperson must pass local
tests demonstrating that it can:

- resolve a handle to a DID and identify the PDS service;
- explain the difference between a DID and a handle;
- parse an AT-URI into DID, collection and record key;
- classify an XRPC call as query or procedure from its Lexicon;
- track a repository revision and firehose sequence independently;
- detect a gap and request a bounded repository resync;
- verify a repository commit against the DID signing key, or explicitly mark
  the evidence unverified;
- distinguish a record delete from a missing observation;
- explain why an App View's result is not itself repository authority.

The first implementation slice should add a protocol evidence ledger and a
read-only `protocol` inspection command. Network acquisition and verification
belong in Wolfram; durable evidence, replay and inspectable explanations belong
in atperson's runtime/core boundary.
