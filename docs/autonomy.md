# Autonomous runtime contract

ATperson's autonomous layer is responsible for keeping its local runtime
healthy after bootstrap: starting bounded perception cycles, recovering from
crashes, preserving replayable state, applying resource budgets, and stopping
 safely on operator or safety signals.

## Bootstrap boundary

The first launch calls the idempotent C bootstrap before loading learned state.
It creates the private data directory and an environment template without
overwriting existing files. Bootstrap does not create credentials, publish
records, or enable outbound writes.

## Runtime ownership

The runtime lifecycle is checkpointed locally in `autonomy-run.json` under the
configured central data directory. `atperson autonomy status` exposes the last
run id, phase, checkpoint number, and diagnostic detail. Daemon startup records
`recovering`, successful bounded AT Protocol perception records `learning`,
and graceful shutdown records `stopped`. This state is runtime metadata only;
it is never added to the learned graph or sent to a service.

The daemon owns the long-lived writer lock and runs bounded sync cycles. Each
cycle checkpoints durable state only after successful processing, uses bounded
backoff on transport failures, and responds to pause, resume, shutdown, and
resource-pressure controls. A restart reconstructs state from the snapshot and
replayable ledgers rather than trusting transient process memory.

The autonomy supervisor must maintain these invariants:

- no write occurs merely because learning, planning, or recovery succeeded;
- every outbound action passes policy, rate budget, dry-run, pause, and exact
  approval gates;
- OAuth sessions are DPoP-bound and scope-limited; credentials never enter the
  learned model or protocol evidence ledger;
- protocol evidence and social learning remain separate durable stores;
- malformed, unverified, or rejected protocol input remains inspectable;
- shutdown and emergency pause are fail-closed and recoverable.

External publishing also has an independent environment master switch:
`ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true`. Bootstrap writes it as false in the
private `.env` template. After the first interactive ingest completes, the CLI
asks whether the operator wants to enable publishing and prints the exact
change; activation requires sourcing the updated environment and starting a
new process. The switch cannot bypass policy, dry-run, pause, or exact-digest
approval.

## Recovery phases

1. bootstrap and validate private paths;
2. acquire the writer lock;
3. recover torn ledger tails and snapshot commit markers;
4. load runtime control and resource policy;
5. resume bounded protocol/social perception;
6. checkpoint and release state on graceful shutdown.

If recovery cannot establish a trustworthy committed prefix, the supervisor
stops and reports the failure. It must not guess, publish, or discard evidence.

## Standing authorization

Autonomous execution is per-digest approval by default: the operator
approves the exact frozen action, or nothing runs. Standing authorization
envelopes (#141) are the bounded alternative — the operator pre-approves a
class of actions (kinds, ceilings, score floors, scope terms, expiry) and
the scheduler can execute matching proposals without a per-action round
trip.

The contract is unchanged where it matters: an envelope never widens
policy, coverage is re-evaluated from disk at execution time, and
revocation takes effect on the next attempt. See
[`outbound-policy.md`](outbound-policy.md) for the envelope format and
CLI.

The current implementation provides the bootstrap, daemon, control, resource,
ledger-recovery, and outbound-gate primitives. Future autonomous scheduling
must compose those same paths rather than introducing a privileged write API.
