# AT Protocol knowledge boundary

This directory owns AT Protocol vocabulary, structural validation, evidence
reduction, replay and inspection contracts. It is deliberately separate from
the C23 social-content graph: protocol facts never become vocabulary,
preferences, biography or personality state merely because they were observed.

The authoritative external references are:

- [Overview](https://atproto.com/guides/overview)
- [Identity](https://atproto.com/guides/identity)
- [Lexicon](https://atproto.com/specs/lexicon)
- [XRPC](https://atproto.com/specs/xrpc)
- [Repository sync](https://atproto.com/specs/sync)
- [Data repositories](https://atproto.com/guides/data-repos)

Wolfram owns network transport, AT Protocol parsing, identity resolution and
cryptographic verification. `atperson` owns the durable evidence boundary:
every accepted or rejected fact retains its source, event type, sequence or
timestamp, verification state and confidence. Unknown or invalid input is
retained as inspectable unverified/rejected evidence; it is not silently
converted into learned social content.

The reducer keeps repository revisions separate from firehose sequence cursors.
A cursor gap creates a bounded resynchronization plan and pauses advancement
until a validated repository/CAR resync is explicitly committed. App View
observations are marked derived and never treated as repository authority.

Live Jetstream `#commit` frames may feed the social-content pipeline only after
the ordinary ingestion policy. `#sync`, `#identity`, `#account`, and account
deletion frames are protocol-only: their raw Wolfram payloads are written to
the evidence ledger with sequence and provenance, and they are never passed to
the social learner. Archive replay uses the same evidence boundary.

Run `atperson protocol status` to inspect the local evidence ledger, or
`atperson protocol explain <subject>` to inspect provenance for one subject.
