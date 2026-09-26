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

### The kill-the-host drill

The procedure above, run end to end, is the drill. Do it on a real
deployment periodically and after any change to the publication or
reconstruction path: it is the only test that proves the network copy is
actually sufficient. The steps below are ordered so that a failure names
the stage that broke rather than "recovery did not work".

Preconditions: entity credentials in the environment
(`ATPERSON_IDENTIFIER`, `ATPERSON_APP_PASSWORD`, `ATPERSON_SERVICE`),
`writes_enabled` on (`atperson control writes on` — the drain is an
outbound write and the gate is fail-closed), and a **stopped** daemon so
nothing races the wipe. Record the current state first; that record is the
pass criterion.

```sh
# 0. Baseline the host you are about to destroy.
atperson control status
atperson stats                 # observations, vocabulary, associations
atperson journal valence       # self-authored experience the ledger lacks
atperson statepub status       # publication backlog
```

If `statepub status` reports a backlog, drain it now (step 1) and check it
is empty: an un-drained prefix exists only on the host you are about to
lose, and the drill cannot recover it.

```sh
# 1. Publish. Only the network copy survives the next step.
atperson statepub drain        # repeat until statepub status is caught up
atperson statepub status
```

```sh
# 2. Record a self-authored valence event the ledger alone would lose.
atperson journal apply <token> action 0.75 <source-uri>
atperson journal valence
atperson statepub drain        # the journal drains on the same pass
```

```sh
# 3. Kill the host. Do this for real; a copy is not a drill.
docker compose down -v         # or: the host's data directory, deleted
```

On the replacement host, with credentials in the environment and
`ATPERSON_HOME` pointing at an **empty** directory:

```sh
# 4. Reconstruct from the network alone. Reconstruct never merges.
atperson reconstruct --into ./recovered
```

Exit 0 only when nothing failed. The report line gives replayed/skipped/
failed counts per kind plus `records_corrupt`. Any failure is printed; stop
here and read it — a corrupt or unverifiable record is a real problem, not
something to retry past.

```sh
# 5. Put the recovered files at their configured paths.
#    ledger.bin (ATPERSON_LEDGER), action-journal.jsonl
#    (ATPERSON_ACTION_JOURNAL) and thoughts/ under the data directory.

# 6. Retrain. The model is never published; it is replayed.
atperson rebuild
atperson stats
```

Pass criterion: `stats` and `journal valence` match the step-0 record.
The learned state — observations, vocabulary, associations, valence — came
back from the network and nothing else.

The recovered host starts closed (above). Re-enable writes deliberately,
not as part of the drill:

```sh
atperson control status         # expect writes off, dry-run on, approval required
atperson control writes on
atperson control dry-run off    # if the deployment is not approval-gated
```

What the drill cannot cover, and why:

- **Credential storage.** Credentials are host-local and are never
  published. Losing them loses nothing recoverable, but a new host without
  them cannot read or write the repository; have a second one before you
  need it.
- **The un-drained backlog.** Only the last drain is the recovery
  boundary. Step 1 is not optional.
- **Content availability.** Observations carry provenance and a digest,
  never third-party text. Reconstruction re-fetches from the source URI
  and verifies the digest, so a source that has since been deleted is
  reported as failed and not replayed. Fewer replayed observations than
  step 0 recorded is a *content* loss, and the report says so.
- **Learned model bytes.** They are not published; the rebuild in step 6
  regenerates them by replay. Snapshot bytes may differ from the lost
  host's; learned state must not.

This drill is pinned automatically as an end-to-end scenario against an
in-memory fake PDS — `tests/recovery/host_loss.cpp`, run by the `atperson-e2e`
test. It also asserts the boundary case: a host that lost an un-drained
backlog recovers a *shorter* history and the scenario requires the
difference to be visible rather than silently matching.

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
