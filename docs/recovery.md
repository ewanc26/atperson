# Operator recovery runbook

How to check that the entity is alive, and how to recover it. The local
observation ledger is the commit authority; the AT Protocol records
published under the entity's own DID make it reconstructable. Every path
below is fail-closed: none of them authorizes an outbound write.

## Entity health

The supervisor health surface (#143, #168) is a heartbeat document the
daemon refreshes atomically at the end of every cycle, plus a poll
command with exit-code semantics:

```sh
atperson autonomy health
```

Output is one JSON line — `health` (`healthy` / `stale` / `unreadable`),
the last `run_id`, `cycle`, `beat_at`, `phase`, and a `detail` string.
Exit codes: **0** healthy, **1** stale, **2** unreadable or never
started. A watchdog restarts on non-zero.

- The heartbeat lives at `<data>/autonomy-heartbeat.json`
  (`ATPERSON_AUTONOMY_HEARTBEAT`), written only after a completed cycle.
  A fresh install reports `unreadable` until the first cycle finishes.
- Staleness is wall-clock age of `beat_at` against a threshold defaulting
  to 15 minutes; override with `ATPERSON_HEALTH_MAX_AGE_SECONDS`. The
  daemon's default poll interval is 5 minutes
  (`ATPERSON_DAEMON_POLL_MS`), so a hung or spinning daemon goes stale
  within roughly one threshold.
- `unreadable` distinguishes the two failure shapes: no heartbeat file
  (never completed a cycle) or a present-but-corrupt file. Both are
  fail-closed.
- The heartbeat is advisory liveness evidence only. It never
  authorizes anything; the control write gate and approval gates do.

For phase and progress rather than liveness:

```sh
atperson autonomy status
```

reports the run id, lifecycle phase, checkpoint number, last activity and
the count of pending scheduler proposals.

## Lost or corrupt local state directory

If the data directory is lost or corrupt, rebuild from the network. The
entity's durable experience — observations, actions, valence, thoughts,
intents — is published as records under its own DID
(`click.croft.atperson.*`, see [`network-state.md`](network-state.md));
`reconstruct` rebuilds fresh state from those records.

1. Create a fresh, empty directory. Reconstruct never merges into
   existing state.
2. Run the rebuild from the records (network required; credentials come
   from the environment, never from the lost directory):

   ```sh
   ATPERSON_IDENTIFIER=… ATPERSON_APP_PASSWORD=… \
       atperson reconstruct --into ./recovered
   ```

   The report line gives replayed/skipped/failed counts per kind plus
   `records_corrupt`; every failure is also printed. Exit 0 only when
   nothing failed.
3. Place the recovered files at the configured paths: `ledger.bin`
   (`ATPERSON_LEDGER`), `action-journal.jsonl`
   (`ATPERSON_ACTION_JOURNAL`), and the `thoughts/` directory under the
   data directory. Nothing else is needed — runtime metadata
   (cursors, control state, heartbeat, run state) is host-local and
   regenerates.
4. Retrain the model from the recovered ledger:

   ```sh
   atperson rebuild
   ```

What is and is not recovered:

- Observation records carry provenance and a content digest, never
  third-party text. Reconstruction re-fetches content from the source
  URI and verifies the digest before replaying; an unavailable source or
  a digest mismatch is reported and **not** replayed. A tampered record
  cannot enter the ledger.
- Withdrawn observations stay excluded: the publisher republishes them
  with `outcome: "withdrawn"` and reconstruct skips them, so a rebuild
  honours withdrawal.
- Intents (#161) replay into the journal and restore pending
  conversations.
- Journal events and learned model bytes are not published; the model
  is rebuilt by replay. Credentials are host-local and never published.

Reconstruction is only as current as the last `statepub drain`. If the
host was lost with a publication backlog, the un-drained prefix exists
only in the lost directory and is gone; the network copy is the
recovery boundary.

## Host loss

The PDS is the backup. A standing deployment on a new host is the same
path as above: install, set `ATPERSON_HOME`, provide credentials via the
environment, `reconstruct --into` a fresh directory, move the recovered
files into place, `rebuild`.

A fresh host starts closed. A missing control file means writes are
disabled, dry-run is on and approval is required — the fail-closed
defaults — so the recovered entity learns but publishes nothing until
the operator explicitly re-enables writes
(`atperson control writes on`, plus policy and approvals as before).

## Lifecycle checkpoint recovery

The daemon checkpoints its lifecycle in `<data>/autonomy-run.json`
(`ATPERSON_AUTONOMY_RUN_STATE`): run id, phase
(recover → learn → propose → approval → execute → verify →
stopped/failed), checkpoint counter and diagnostic detail. Startup
records `recovering`, the first completed cycle records `learning`, and
graceful shutdown records `stopped`.

- **Missing file**: treated as a fresh run — defaults apply and the
  daemon starts normally. The checkpoint is runtime metadata, not
  authority; a missing checkpoint must not and does not authorize an
  external action. Outbound writes remain governed by the control gate
  and approval gates, which fail closed independently.
- **Corrupt file**: `load_autonomy_run_state` throws, the command exits
  non-zero, and no work proceeds — fail-closed. To resolve, inspect the
  file, then remove or rename it. Only run history and diagnostics are
  lost; learned state lives in the ledger and is untouched.
- **Torn durable writes** from a crash are self-healing: a torn ledger
  tail beyond the committed offset is truncated on load, and a torn
  final journal line is truncated and reported. A fenced prefix that
  fails validation is corruption, not a torn tail — recover via the
  network reconstruct path above.
- If recovery cannot establish a trustworthy committed prefix, the
  supervisor stops and reports. It must not guess, publish or discard
  evidence; use the reconstruct path instead.

## Offline spool drain

Offline mode (#154) makes outbound writes offline-safe. With
`atperson control offline on`, an approved outbound attempt does not
touch the network: the exact serialized action document is appended to
`<data>/offline-spool/pending/` (`ATPERSON_OFFLINE_SPOOL`) and the
attempt reports dry-run with reason `offline_spooled`. Learning,
planning and journalling continue unchanged. A transport failure during
a live write also leaves the entry spooled — implicit offline.

Returning to online needs no manual drain step:

```sh
atperson control offline off
```

With the scheduler enabled (`ATPERSON_SCHEDULER=1`), each cycle drains
the spool first, in creation order, through the same attempt atom as a
live write: policy, budget, dry-run, approval and audit all re-run
against the *current* control state, and reply CIDs resolve online at
drain time. A spooled action approved yesterday is re-judged today; a
since-denied action moves to `denied/` permanently. The drain is bounded
per cycle and stops at the first transport failure, resuming on the next
cycle.

Outcomes per entry: executed → removed; denied → moved to `denied/`
(never retried); dry-run/deferred → stays pending; failed → drain stops,
entry stays spooled.

Inspection is read-only:

```sh
atperson outbound spool
```

reports offline mode, pending count, earliest pending time, denied
count and the last assigned sequence.

This spool is distinct from state-publication offline staging:
`atperson statepub drain --offline` stages record JSON under
`<data>/replicate/records/` and shares the publication cursor, so a
later online drain resumes from the same point. See
[`network-state.md`](network-state.md).

## Container healthcheck

The Compose deployment wires the health surface directly
(see [`docker.md`](docker.md)):

```yaml
healthcheck:
  test: ["CMD", "atperson", "autonomy", "health"]
```

Exit 1 (stale) and 2 (unreadable) both fail the check, matching the
watchdog contract. `start_period` covers the gap before the first
completed cycle writes a heartbeat; raise it if first-run catch-up is
long.
