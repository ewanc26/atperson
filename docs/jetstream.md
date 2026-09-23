# Jetstream ingestion

`atperson jetstream` is the unauthenticated public streaming-ingestion path. It consumes Bluesky Jetstream JSON events through Wolfram and funnels supported records into the same observation ledger and learning path used by authenticated timeline sync.

Jetstream is derived from the AT Protocol repository firehose, but it is not the binary `com.atproto.sync.subscribeRepos` wire format. atperson does not implement a second AT Protocol stack: Wolfram owns the WebSocket/Jetstream mechanics.

## Current command

```sh
atperson jetstream [max-events] [max-ms] [--collections <file>] [--dids <file>]
atperson jetstream archive [after-seq] [before-seq] [--span <sequences>] [--collections <file>] [--dids <file>]
atperson jetstream status [--collections <file>] [--dids <file>]
```

The positional limits bound one live invocation by event count and wall-clock milliseconds. A zero/unset limit remains unbounded in that dimension.

The default public endpoint is:

```text
wss://jetstream.us-east.bsky.network/xrpc/network.bsky.jetstream.subscribeEvents
```

Override it with `ATPERSON_JETSTREAM_ENDPOINT` for another public or self-hosted
Jetstream service. The canonical `subscribeEvents` endpoint enables v2 sequence
cursors and is required for archive-to-live cutover; the legacy `/subscribe`
endpoint remains available for explicitly configured live-only consumers.

## Status inspection

```sh
atperson jetstream status [--collections <file>] [--dids <file>]
```

This command is read-only and runs before the learned model is loaded. It reports the effective Jetstream endpoint, current runtime phase, dedicated state-file path, persisted cursor/checkpoint generation, and the collection/DID filters that would be used by a live run.

The phase reports `live-only` when no daemon startup window is configured, and `archive-startup` when the daemon will replay before timeline cycles. The replay planner, sealed-segment decoder, bounded-window fetcher, archive command, relative-span surface, daemon scheduling and archive-to-live checkpoint integration are all implemented.

The bounded operator command is now available:

```sh
atperson jetstream archive [after-seq] [before-seq]
```

The archive command also accepts `--collections <file>` and `--dids <file>`;
when omitted it uses the same configured filter files as live Jetstream. These
filters are passed to the replay planner and are never inferred from records.

It authenticates the archive API with `ATPERSON_JETSTREAM_ARCHIVE_TOKEN` (a raw
Jetstream archive token; it is never persisted or logged) and uses the configured
Wolfram session for service setup. It defaults `after-seq` to the
persisted Jetstream checkpoint (or zero), and optionally caps the window at an
inclusive `before-seq`. A relative trailing window can be requested instead
with `--span <sequences>`: atperson probes the archive's sealed tip (or uses an
explicit `before-seq`) and plans the trailing span ending there. Every
invocation is hard-capped to a 10,000,000-sequence
window. Successful completion checkpoints the sealed replay tip
through the same durable ingestion path. The same token is required when the
daemon startup archive phase is enabled via `ATPERSON_DAEMON_ARCHIVE_AFTER`,
`ATPERSON_DAEMON_ARCHIVE_BEFORE` or `ATPERSON_DAEMON_ARCHIVE_SPAN`.

## Entity identity and policy parity

Live Jetstream ingestion is unauthenticated and can bootstrap without credentials. If `ATPERSON_SELF_DID` is set to a non-secret DID such as `did:plc:...`, it is used only to apply the same self-authored exclusion as authenticated timeline polling; when unset, the public tail remains available but that exclusion cannot be applied. Archive replay additionally requires a Wolfram session and archive token (see above).

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

The current live client stores the Jetstream cursor exposed by the pinned Wolfram live API (v2 hosts accept the legacy timestamp form on the live tail, so it remains restart-compatible). Native v2 replay is sequence-based. The archive command persists the sealed replay tip `seq` as the live resume point after a successful cutover, so the archive and live paths share one checkpoint file.

## Collection filter

Without an explicit filter file, atperson subscribes to every public
collection and accepts every public Jetstream event kind. This provides a
complete protocol-evidence stream; the social-learning policy still trains
only supported public text records.

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

A transport filter does not automatically make a new record kind learnable.
The existing extraction/ingestion policy remains authoritative; today only
public `app.bsky.feed.post` records become post observations. Other public
records remain protocol evidence, while private-message payloads are never
accepted as social-learning input.

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

## #60 status

The relative backfill window was the last piece of the #60 operator surface; it is implemented:

```sh
atperson jetstream archive --span 2000000
```

With `--span`, atperson probes the archive's sealed tip first, then plans the
trailing `<span>` sequences ending at that tip (or at an explicit positional
`before-seq`). The span is bounded by the same 10,000,000-sequence cap as
absolute windows (`kJetstreamArchiveMaxSequenceSpan`); `--span` cannot be
combined with a positional `after-seq`. The command still
runs the same plan/fetch/translate/checkpoint path and can cut over to the live
tail without a gap.

The daemon accepts the same relative window through
`ATPERSON_DAEMON_ARCHIVE_SPAN` (mutually exclusive with
`ATPERSON_DAEMON_ARCHIVE_AFTER`) for its startup replay phase. Replay-window
truncation, restart, withdrawal, failure and relative-span behavior are covered
by scripted tests that exercise the replay source seam without credentials or
network I/O. The live tail remains a bounded cursor consumer with independent
restart state and explicit collection filtering.
