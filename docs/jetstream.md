# Jetstream ingestion

`atperson jetstream` is the unauthenticated public streaming-ingestion path. It consumes Bluesky Jetstream JSON events through Wolfram and funnels supported records into the same observation ledger and learning path used by authenticated timeline sync.

Jetstream is derived from the AT Protocol repository firehose, but it is not the binary `com.atproto.sync.subscribeRepos` wire format. atperson does not implement a second AT Protocol stack: Wolfram owns the WebSocket/Jetstream mechanics.

## Current command

```sh
atperson jetstream [max-events] [max-ms] [--collections <file>] [--dids <file>]
```

The positional limits bound one invocation by event count and wall-clock milliseconds. A zero/unset limit remains unbounded in that dimension.

The default public endpoint is:

```text
wss://jetstream.us-east.bsky.network/subscribe
```

Override it with `ATPERSON_JETSTREAM_ENDPOINT` for another public or self-hosted Jetstream service.

## Status inspection

```sh
atperson jetstream status [--collections <file>] [--dids <file>]
```

This command is read-only and runs before the learned model is loaded. It reports the effective Jetstream endpoint, current runtime phase, dedicated state-file path, persisted cursor/checkpoint generation, and the collection/DID filters that would be used by a live run.

The phase currently reports `live-only`. Once Wolfram #36 exposes Jetstream v2 archive replay, this surface is where the replay/backfill phase and archive-to-live cutover state should become visible rather than being hidden inside the transport.

## Entity identity and policy parity

Live Jetstream ingestion requires `ATPERSON_SELF_DID`, a non-secret DID such as `did:plc:...`. Jetstream itself remains unauthenticated; the DID is used only to apply the same self-authored exclusion as authenticated timeline polling.

If the DID is missing, `atperson jetstream` refuses before connecting or mutating learned state. `atperson jetstream status` remains available and reports that the self DID is missing.

The record-level policy is shared with polling: self-authored posts are skipped, replies and quotes retain their eligible reason, empty/media-only records are classified consistently, and unsupported/private record classes remain non-learnable. Viewer-specific mute/block/moderation fields are only available on AppView timeline responses; public Jetstream events do not fabricate those viewer-state signals.

## Independent checkpoint

Timeline polling and Jetstream use different runtime state files:

```text
timeline:   <data>/ingestion-state.json
jetstream:  <data>/jetstream-state.json
```

`ATPERSON_INGESTION_STATE` and `ATPERSON_JETSTREAM_STATE` override them independently.

This separation is required for #60: switching from polling to Jetstream must not discard the polling cursor, and a later polling run must not discard the Jetstream cursor. Both paths still converge on the same durable observation ledger, whose deduplication is authoritative.

The Jetstream cursor is operational metadata only. It is never written into the model snapshot and is not evidence that an event was learned.

The checkpoint is also bound to the exact Jetstream WebSocket endpoint. If `ATPERSON_JETSTREAM_ENDPOINT` changes, atperson deliberately starts that stream from a fresh cursor instead of assuming two servers share one cursor namespace. Any overlapping records are still suppressed by the shared observation ledger.

The current live client stores Jetstream's envelope microsecond timestamp because that is the cursor exposed by the pinned Wolfram live API. Jetstream v2 deliberately accepts this legacy timestamp form on the live tail, so it remains restart-compatible on the v2 host. Native v2 replay is sequence-based; once Wolfram #36 lands, archive planning/cutover must persist and expose the v2 `seq` cursor rather than pretending the timestamp cursor is an archive position.

## Collection filter

Without an explicit filter file, atperson subscribes only to:

```text
app.bsky.feed.post
```

Pass a file directly:

```sh
atperson jetstream --collections ./collections.txt
```

or configure a default:

```sh
export ATPERSON_JETSTREAM_COLLECTIONS_FILE="$HOME/.ewanc26/atperson/jetstream-collections.txt"
```

The file is plain text, one Jetstream `wantedCollections` value per line:

```text
# public posts
app.bsky.feed.post

# a wildcard is passed through to Jetstream
app.bsky.graph.*
```

Blank lines and `#` comments are ignored. Duplicate filters are removed while preserving first occurrence order. More than 100 unique filters, an empty file, or a token containing whitespace is rejected before connecting.

A transport filter does not automatically make a new record kind learnable. The existing extraction/ingestion policy remains authoritative; today only public `app.bsky.feed.post` records become post observations.

DID filtering is optional. Pass `--dids <file>` or set `ATPERSON_JETSTREAM_DIDS_FILE`. The file uses the same blank/comment/dedup rules, requires every entry to begin with `did:`, and accepts at most 10,000 unique values. With no DID file, the subscription is not restricted by repository DID.

Both collection and DID files are transport policy only. They are not model state and are not persisted into the learned snapshot.

## Durability order

For every supported event, Jetstream uses the same durable ordering as polling:

```text
ledger reservation (pending)
        ->
remember / learn
        ->
ledger outcome commit
        ->
action-event linkage
        ->
cursor checkpoint
```

The cursor is saved only after the events represented by it have been durably handled. On restart, ledger deduplication prevents already-committed observations from training twice.

Malformed Jetstream frames are counted against the `max-events` work budget and skipped rather than terminating the stream. Because an unparseable envelope has no trustworthy cursor, atperson does not fabricate one; the next valid frame advances the cursor. The CLI reports the number of malformed frames skipped.

## Privacy and network effects

Jetstream is public, read-only ingestion. It uses no account login or app password.

It does not enable posts, replies, likes, follows, reposts, DMs or moderation. Those remain behind the separate outbound policy/control/write path.

The learning policy still accepts only public post records. Private-message/conversation payloads are not learning input.

## What remains for #60

This is not the complete #60 implementation.

Jetstream v2 now provides archive replay that can backfill a historical slice and cut over to the live tail without a gap. atperson does not yet drive that archive API. The remaining work is to add:

- an operator-selected bounded relative backfill window;
- Jetstream v2 archive planning/segment retrieval through Wolfram;
- a deterministic archive-to-live cutover;
- inspection/status for the active collection filter, replay window and phase;
- tests for replay-window truncation and restart across the archive/live boundary.

Until then, `atperson jetstream` is a bounded live/cursor consumer with independent restart state and explicit collection filtering.
