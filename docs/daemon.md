# Ingestion daemon

`atperson daemon` is the long-running runtime stage: repeated bounded sync
cycles over the same durable ledger, snapshot and ingestion cursor that
`sync` uses. It adds scheduling, retry backoff, snapshot cadence and graceful
shutdown — it does not add new learning.

It is deliberately thin around the existing pieces:

- **C23** still owns all learned state and the ledger replay/withdrawal
  semantics.
- **The C++ sync engine** still processes each page durably before advancing
  the cursor; the ledger, not the cursor, is the authority for what was
  learned.
- **Wolfram** still owns AT Protocol mechanics; the daemon only drives the
  same authenticated timeline fetches `sync` does.

The loop itself (`src/app/daemon/`) has no network, Wolfram or clock
dependency. Transport, sleeping, stopping and persistence are injected, so
the whole control flow is exercised offline in `tests/daemon/`.

## Command

```sh
export ATPERSON_IDENTIFIER="handle.example"
export ATPERSON_APP_PASSWORD="xxxx-xxxx-xxxx-xxxx"

./build/atperson daemon          # run until stopped
./build/atperson daemon 5        # stop after 5 cycles (overrides the env bound)
```

The daemon holds the state-directory writer lock for its **entire lifetime**,
not just per cycle. Another process must never load a stale in-memory
snapshot and later clobber this one.

## Cycle model

Each cycle runs the existing bounded sync traversal. At the end of a cycle
the daemon:

1. persists the ingestion cursor (`save_ingestion`), always;
2. snapshots the graph on the configured cadence when the graph is dirty,
   where *dirty* means the cycle learned or ledgered anything;
3. waits — the poll interval after a cycle that drained the timeline, the
   catch-up interval while catch-up is still pending.

On a graceful stop (signal or operator shutdown request) it flushes the
cursor and any dirty snapshot before returning. On a fatal persistence error
it stops immediately and does **not** attempt a flush.

## Failure semantics

Only `RetryableError` is retried. The CLI fetch wrapper classifies
transport/HTTP failures from Wolfram as `RetryableError`; anything else
(a local persistence failure, a corrupt state file) is fatal and propagates
out of `run_daemon`, leaving the process with a clear error rather than
continuing on uncertain durable state.

Retries use bounded exponential backoff with deterministic jitter. The delay
sequence is reproducible for a fixed config and seed.

A mid-traversal transport failure leaves `run_sync`'s ingestion state
unadvanced. The next attempt refetches the same page and ledger deduplication
suppresses anything already committed — so restart after an abrupt
termination trains nothing twice.

## Operator control

`atperson control` (pause, resume, write gate, dry-run, approval, shutdown)
stays operable while the daemon runs. Control state is runtime metadata, not
part of the snapshot/ledger/cursor state set, so it does not take the writer
lock. The daemon re-reads it every cycle:

- `control pause` defers cycles (sleep, re-check) without exiting;
- `control shutdown` asks the daemon to flush and exit;
- a startup pause refuses a new daemon outright.

SIGINT/SIGTERM interrupt a poll or catch-up wait immediately so shutdown is
not delayed to the end of the interval.

## Configuration

All knobs are optional environment overrides. Page size and the observation
budget remain owned by the resource budget
(`ATPERSON_SYNC_PAGE_SIZE`, `ATPERSON_SYNC_MAX_OBSERVATIONS`), matching
`sync`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATPERSON_DAEMON_PAGES_PER_CYCLE` | `1` | pages traversed per cycle |
| `ATPERSON_DAEMON_POLL_MS` | `300000` | wait after a drained cycle |
| `ATPERSON_DAEMON_CATCHUP_MS` | `0` | wait while catch-up is pending |
| `ATPERSON_DAEMON_MAX_CYCLES` | `0` | stop after N cycles (`0` = unbounded) |
| `ATPERSON_DAEMON_SNAPSHOT_EVERY` | `1` | snapshot cadence in cycles |
| `ATPERSON_DAEMON_BACKOFF_INITIAL_MS` | `1000` | first retry delay |
| `ATPERSON_DAEMON_BACKOFF_MAX_MS` | `300000` | retry delay ceiling |
| `ATPERSON_DAEMON_BACKOFF_FACTOR` | `2.0` | exponential retry factor |
| `ATPERSON_DAEMON_BACKOFF_JITTER` | `0.2` | jitter fraction in `[0, 1]` |

Malformed, zero or self-contradictory values fail at startup.

## Boundaries

The daemon is still **read-only** with respect to the network: it ingests
public timeline data into the learning graph. It does not post, reply, like,
follow, repost, or moderate, and it is not a path around the fail-closed
outbound policy. Autonomous output waits on explicit network policy and a
Wolfram-backed write path, not on the daemon.
