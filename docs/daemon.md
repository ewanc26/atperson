# Ingestion daemon

`atperson daemon` is the long-running form of `sync`. It repeatedly runs the same bounded ingestion path over the same ledger, snapshot and cursor, adding scheduling, retry backoff and graceful shutdown without introducing a second learning path.

The ownership stays the same:

- C23 owns learned state and ledger/replay semantics;
- the C++23 sync engine owns orchestration around each page;
- Wolfram owns AT Protocol fetching;
- the ledger, not the cursor, remains the authority for what was actually committed.

The daemon loop in `src/app/daemon/` is written so its transport, clock, sleep and persistence boundaries can be injected and tested offline.

## Running it

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"

./build/atperson daemon
./build/atperson daemon 5
```

Without a cycle count it runs until stopped. A numeric argument bounds the run and overrides the environment limit.

The daemon owns the data-directory writer lock for its entire lifetime. That prevents a second mutating process from loading stale model state and later overwriting newer state.

## Cycle model

Each cycle runs the existing bounded sync traversal. Afterwards the daemon:

1. saves the ingestion cursor;
2. saves a dirty model snapshot on the configured cadence;
3. waits for either the poll interval or catch-up interval before the next cycle.

A cycle is dirty when it learned or ledgered anything.

On a graceful stop, the daemon flushes the cursor and any dirty snapshot before exiting. A fatal local persistence error stops immediately instead of attempting to continue from uncertain state.

## Retry behaviour

Only transport/HTTP failures classified as `RetryableError` are retried. Local persistence errors, corrupt state and other fatal failures propagate out of the loop.

Retries use bounded exponential backoff with deterministic jitter. For a fixed configuration and seed, the delay sequence is reproducible.

If a transport failure happens part-way through traversal, the cursor for that page is not advanced. The next attempt fetches the same page and ledger deduplication suppresses observations that were already committed. Restarting therefore does not silently train the same committed post twice.

## Operator control

`atperson control` remains usable while the daemon holds the writer lock because control state is separate runtime metadata.

- `control pause` makes the loop wait and re-check instead of ingesting;
- `control resume` clears that pause;
- `control shutdown` asks for a graceful flush and exit;
- a daemon started while already paused refuses to start.

SIGINT and SIGTERM interrupt waits immediately rather than waiting for the next polling boundary.

The outbound write gate, dry-run mode and approval state are also operator control metadata. They do not turn the ingestion daemon into a network-writing process.

On a headless host there is no shell to run those subcommands from. When `ATPERSON_OPERATOR_DID` is set, the daemon also runs one bounded remote-control poll per cycle, before the scheduler, and applies operator-authored AT Protocol records to the same control state. It is inert — and opens no session — while that variable is unset. The operator DID must differ from the entity's own account DID: if they match the pass is refused and the daemon says so each cycle, because an entity that can command itself has no operator. See [`remote-control.md`](remote-control.md).

## Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATPERSON_DAEMON_PAGES_PER_CYCLE` | `1` | Pages traversed in one cycle |
| `ATPERSON_DAEMON_POLL_MS` | `300000` | Delay after a drained cycle |
| `ATPERSON_DAEMON_CATCHUP_MS` | `0` | Delay while catch-up remains pending |
| `ATPERSON_DAEMON_MAX_CYCLES` | `0` | Stop after N cycles (`0` means unbounded) |
| `ATPERSON_DAEMON_SNAPSHOT_EVERY` | `1` | Snapshot cadence in cycles |
| `ATPERSON_DAEMON_BACKOFF_INITIAL_MS` | `1000` | First retry delay |
| `ATPERSON_DAEMON_BACKOFF_MAX_MS` | `300000` | Maximum retry delay |
| `ATPERSON_DAEMON_BACKOFF_FACTOR` | `2.0` | Exponential multiplier |
| `ATPERSON_DAEMON_BACKOFF_JITTER` | `0.2` | Jitter fraction in `[0, 1]` |
| `ATPERSON_DAEMON_ARCHIVE_AFTER` | unset | Run a Jetstream archive phase before timeline cycles, starting at this sequence |
| `ATPERSON_DAEMON_ARCHIVE_BEFORE` | unset | Optional upper bound for the daemon archive window |
| `ATPERSON_DAEMON_ARCHIVE_SPAN` | unset | Relative trailing window for the archive phase (mutually exclusive with `AFTER`) |

The Jetstream archive startup phase is opt-in and covered in [`jetstream.md`](jetstream.md). When one of the `ATPERSON_DAEMON_ARCHIVE_*` variables is set, the daemon runs a bounded sealed-archive replay over the configured window before the first timeline cycle, checkpoints the replay tip and model, then continues normally. The window is always hard-capped at 10,000,000 sequences (`kJetstreamArchiveMaxSequenceSpan`); `ATORSPAN`/`AFTER` combinations that exceed it fail at startup.

Sync page size and observation limits continue to come from the resource-budget layer. Invalid or self-contradictory daemon settings fail at startup.

## Network boundary

The daemon reads public timeline data. It does not autonomously post, reply, like, follow, repost, message or moderate.

Operator-led posts and replies go through the separate `publish` path described in [`outbound-execution.md`](outbound-execution.md), with outbound policy, control gates and audit logging in front of the Wolfram write.