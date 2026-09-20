# Protocol literacy acceptance matrix

This matrix maps the required protocol-literacy checks to their authoritative
tests or runtime evidence. It is maintained with the protocol subsystem and is
not part of social learning state.

| Requirement | Evidence |
| --- | --- |
| Handle → DID → PDS | Live `atperson protocol resolve bsky.app`; `protocol status` persists normalized identity and raw DID-document evidence. |
| DID versus handle | `protocol/tests/protocol.cpp`: `accept_identity` rejects a handle in the DID field and accepts distinct DID/handle values. |
| AT-URI, CID, TID, strong references, blobs | `protocol/tests/protocol.cpp` parser assertions. |
| Lexicon/XRPC query versus procedure | `protocol/tests/protocol.cpp`: `classify_xrpc` and `accept_lexicon_fact`. |
| Repository revision versus firehose cursor | `protocol/tests/protocol.cpp` cursor/revision assertions; `tests/sync/jetstream_archive.cpp` checks persisted sequence and revision evidence separately. |
| Cursor gaps and bounded resync | `protocol/tests/protocol.cpp` checks gap detection, bounded plan, rejected incomplete completion, and verified completion. |
| Valid and invalid repository signatures | `tests/atproto/repository_verifier.cpp` exercises valid CARs, tampering, malformed metadata, and unavailable transport. |
| Deletion versus missing observation | `protocol/tests/protocol.cpp` checks `Deleted`, `Missing`, and `Unverified` states; archive replay tests withdrawal separately. |
| App View versus repository authority | `protocol/tests/protocol.cpp` checks `AppViewDerived` versus `Repository` authority. |
| Deterministic duplicate/reconnect/resync replay | `protocol/tests/protocol.cpp` replays duplicate commit and resync evidence twice and compares the resulting stores; sync/archive tests cover ledger deduplication across restart. |

The network-disabled build is also required to pass the core, C++, protocol,
replay, and sync tests. Public learning remains read-only; autonomous
publishing is not enabled by protocol evidence ingestion.

Bounded JetStream archive replay is implemented through Wolfram's native
replay API and is covered by `tests/sync/jetstream_archive.cpp`. It remains
explicitly gated at runtime: archive replay requires the configured archive
token, a valid bounded sequence window, and an authenticated Wolfram client.
Without those credentials the public path remains live-only and must not claim
that an archive was replayed. The archive API is used only to recover a
bounded gap/startup window; it does not turn learning into an autonomous
publisher.
