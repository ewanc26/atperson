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

## Recovery phases

1. bootstrap and validate private paths;
2. acquire the writer lock;
3. recover torn ledger tails and snapshot commit markers;
4. load runtime control and resource policy;
5. resume bounded protocol/social perception;
6. checkpoint and release state on graceful shutdown.

If recovery cannot establish a trustworthy committed prefix, the supervisor
stops and reports the failure. It must not guess, publish, or discard evidence.

The current implementation provides the bootstrap, daemon, control, resource,
ledger-recovery, and outbound-gate primitives. Future autonomous scheduling
must compose those same paths rather than introducing a privileged write API.
