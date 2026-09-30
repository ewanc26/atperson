# Jetstream ingestion

`atperson jetstream` is the unauthenticated public streaming-ingestion path. It consumes Bluesky Jetstream JSON events through Wolfram and funnels supported records into the same observation ledger and learning path used by authenticated timeline sync.

Jetstream is derived from the AT Protocol repository firehose, but it is not the binary `com.atproto.sync.subscribeRepos` wire format. atperson does not implement a second AT Protocol stack: Wolfram owns the WebSocket/Jetstream mechanics.

## Current command

```sh
atperson jetstream [max-events] [max-ms] [--collections <file>] [--dids <file>] [--kinds <file>]
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

## Ingesting the whole network

With no `--dids` file (leave `ATPERSON_JETSTREAM_DIDS_FILE` unset) the
subscription is not restricted by repository, so every public post on the
network is a candidate observation. A live run saves the model and resume
cursor when it ends, so run it in bounded chunks:

```sh
set -a; . "$HOME/.ewanc26/atperson/.env"; set +a
scripts/whole-network.sh ./build/atperson 300 1   # binary, chunk seconds, max hours
```

The loop stops after the hour limit (`0` runs until stopped) and idles while
`atperson control pause` is in effect.

Two practical notes:

- Filtered subscriptions skip most of the global sequence, so sequence jumps
  are expected: the protocol cursor advances across them (a regression is still
  a rewind and still requires resync) and they do not block checkpointing. They
  stay visible as reconciliation signals: each batch that jumped appends one
  `#filtered-sequence-jump` evidence entry (`jumps`, `first_after`, `last_to`)
  with `Unverified` status, and `JetstreamRunResult::filtered_sequence_jumps`
  carries the count. Nothing is inferred about the skipped events and no
  repository revision is marked verified. On an unfiltered stream a gap still
  requests reconciliation and holds the checkpoint.
- Neural training cost per post grows steeply with the capacity class. On a
  laptop the auto-selected `large` class trains several seconds per post, far
  below the network's post rate; set `ATPERSON_NEURAL_CAPACITY=baseline` (or
  `capable`) before the first creation or a rebuild to keep up with the feed.
- The protocol evidence ledger normally gains one fsynced entry per commit,
  which reached ~6 GB after a day of whole-network ingestion. Set
  `ATPERSON_PROTOCOL_EVIDENCE=control` to keep only non-commit events (identity, account, sync),
  delete and cursor-gap evidence; routine commits stay durable in the
  observation ledger regardless.

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
Jetstream archive token; it is never persisted or logged) against the archive
host configured by `ATPERSON_JETSTREAM_ARCHIVE_HOST` (default
`https://jetstream.us-west.bsky.network`; the archive API is a separate host
from the PDS, so no PDS session is required for replay). It defaults
`after-seq` to the
persisted Jetstream checkpoint (or zero), and optionally caps the window at an
inclusive `before-seq`. A relative trailing window can be requested instead
with `--span <sequences>`: atperson probes the archive's sealed tip (or uses an
explicit `before-seq`) and plans the trailing span ending there. Every
invocation is hard-capped to a 10,000,000-sequence
window. Successful completion checkpoints the sealed replay tip
 through the same durable ingestion path. The same token is required when the
 daemon startup archive phase is enabled via `ATPERSON_DAEMON_ARCHIVE_AFTER`,
 `ATPERSON_DAEMON_ARCHIVE_BEFORE` or `ATPERSON_DAEMON_ARCHIVE_SPAN`.

## Archive record payloads are DAG-CBOR

Live Jetstream frames carry record payloads as JSON objects, but archive
segments serve the same records as canonical DAG-CBOR. The replay path detects
which form it received and decodes the CBOR into the same record shape the
extractor consumes, so a record learned from the archive is indistinguishable
from one learned live.

Wolfram's parser re-serialises and byte-compares, so only canonical DAG-CBOR
decodes; a payload that is not a canonical map is refused rather than
half-read. Within a record that does decode, a field the JSON shape cannot
represent — a CID link in a `reply` strongRef, a byte string — is skipped
instead of failing the record, so a reply still yields its root and parent URIs.

The payload comes from a remote archive, so it is checked before the parser
sees it. A cheap iterative pre-scan rejects input whose declared shape cannot be
backed by the bytes present (an array or map claiming more children, or a
string more bytes, than remain), input nested deeper than any record is, and
indefinite or reserved encodings, none of which are valid DAG-CBOR. Without it a
payload of a few bytes declaring an array of 2^32-1 children kept the parser
busy for many seconds. Such rows are dropped like any other undecodable payload.

Commit rows the archive returned but that produced no event are counted as
`dropped` in the command summary and on the run result. A large planned window
reporting zero events and zero drops means the archive held nothing worth
learning for the configured filters; a non-zero `dropped` means rows arrived
that atperson could not turn into observations, which is a decode problem to
investigate rather than an empty timeline. A dropped row is never inferred to
have been learned, and a dropped row is never silently counted as progress.


## Entity identity and policy parity

Live Jetstream ingestion is unauthenticated and can bootstrap without credentials. If `ATPERSON_SELF_DID` is set to a non-secret DID such as `did:plc:...`, it is used only to apply the same self-authored exclusion as authenticated timeline polling; when unset, the public tail remains available but that exclusion cannot be applied. Archive replay additionally requires the archive token (see above).

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

## Event-kind filter

The v2 `subscribeEvents` endpoint accepts a `kinds` predicate. Without one,
atperson retains commits plus `#sync`, `#identity` and `#account` events for
the protocol-evidence ledger. Pass `--kinds <file>` or set
`ATPERSON_JETSTREAM_KINDS_FILE` to restrict the subscription:

```text
# repository commits only
commit
```

The file uses the same blank/comment/dedup rules and accepts only the four
Jetstream v2 kinds (`commit`, `identity`, `account`, `sync`), at most four
unique values. The kind filter is transport policy only; it never changes what
a delivered event teaches.

## Compression and throughput

Live ingestion requests dictionary-compressed binary frames by default. The
official Jetstream zstd dictionary is fetched from the public
`network.bsky.jetstream.getZstdDictionary` query on the configured endpoint's
service host before connecting, and passed to Wolfram's connect options. Set
`ATPERSON_JETSTREAM_COMPRESS=0` to stay on uncompressed JSON frames.

If the dictionary fetch fails (offline, non-200, empty body) or the Wolfram
build lacks libzstd, the run continues with uncompressed JSON rather than
failing.

The live loop drains readable frames inside one batch instead of returning to
the caller on every idle poll: a busy feed is consumed continuously, and the
caller only sleeps for advertised reconnect backoff. `max-events` is accounted
across batches, so an event-bounded run stops at the operator's bound.

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

## Bounding disk use (rotation)

Whole-network ingestion grows the ledger and the model without limit. Two
opt-in byte caps (unset or `0` = off) bound them; they are enforced after each
`jetstream` / `jetstream archive` run, after each daemon model save, and on
demand with `atperson rotate`:

- `ATPERSON_LEDGER_MAX_BYTES`: releases the raw text of the oldest
  observations (down to ~80% of the cap). Identity, digest, outcome and dedup
  are kept, so an entry is never re-learned. On disk a released entry has
  `payload_len = UINT32_MAX` and the log header becomes `ATPLDG04` (version 4),
  so builds that predate rotation refuse the ledger outright and fail closed.
- `ATPERSON_MODEL_MAX_BYTES`: prunes the least-observed vocabulary (protecting
  tokens with valence history), remaps edges and episodes, then saves.

`atperson rotate --dry-run` previews a rotation without changing anything: it
prints how many ledger payloads would be released and roughly what the size
would be afterwards, and, for the model, the vocabulary size a first pass would
prune to (or that the cap is below the floor and nothing would be pruned). The
model figure is a single-pass estimate, since valence-protected tokens can keep
it higher. Like other read commands it opens the ledger, so it performs only
the recovery that opening always does.

Both are deliberate forgetting. A `rebuild` cannot regrow released observations
or pruned tokens, and a pruned model is not what replay would produce. Released
entries still cost ~130 B of metadata each, and the model keeps a mirror of it,
so a cap below that floor cannot be reached. `rotate` warns then; for the model it refuses to prune (rather than erode the vocabulary chasing an unreachable cap), and the ledger is not rewritten when no payload is left to release. `ATPERSON_PROTOCOL_EVIDENCE` accepts only `all` or `control`.

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
