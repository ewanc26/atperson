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
replayable ledgers rather than trusting transient process memory. When the
scheduler is enabled, each cycle first runs the idempotent expectation-
resolution pass (#149), so events that landed since the last cycle are judged
before any new decision is made. With intents enabled (`ATPERSON_INTENTS=1`),
the same head of cycle also sweeps the journal's open conversations: the
window-closed and budget-reached states are journaled once and never drive
another decision. Decision contexts that continue an open conversation are
composed as continuation replies under the same policy, rate-budget and
approval gates as the original action (#150).

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
ledger-recovery, and outbound-gate primitives. Autonomous scheduling composes
those same paths rather than introducing a privileged write API.

## Output guard: what an unattended account may say

The text the entity posts on its own is assembled from learned tokens, and the
tokens come from whatever public text it has read. Nothing in the guarded
decision layer says the result is safe to publish unattended, so the scheduler
runs every autonomous post and reply through a deterministic output guard, when
the proposal is composed and again just before it is executed (so a proposal
frozen earlier is checked against today's rules). It is the equivalent of a
conventional agent's output guardrail, with a stable reason for every refusal
and no model in the loop.

Refused, in order: empty text; invalid UTF-8; more than 300 characters (the
platform's post limit); a URL (a scheme, `www.`, a domain-shaped word, or the
words http/https/www); a mention (`@name`, or a DID); a hashtag; a term on the
operator's denylist; and text identical to one already published or queued
within the repeat window (case, spacing and punctuation ignored). URLs,
mentions and hashtags are always refused for autonomous text: they are how an
unattended account becomes a spam source, and nothing the entity learned
justifies them. Repeating the same words is the other classic way, which the
platform's own rules call out ("repeatedly post content"); the existing
duplicate suppression is keyed to the decision, not the words, so two different
contexts that decide the same text would otherwise both go out. An operator
posting through `publish` is not routed through the guard.

The denylist is operator policy, not persona: the entity starts with no
opinions and an empty list, and you decide what it must never say. It is a file,
one term per line (`#` comments), at `ATPERSON_OUTPUT_DENYLIST` or
`<data>/output-denylist.txt`, and it is re-read every cycle, so adding a term
takes effect on the next cycle with no restart and works as a live brake. A
term matches as a whole word, case-insensitively, so `ass` does not block
`class`; a term with a space or punctuation matches as a phrase. The repeat
window is `ATPERSON_OUTPUT_REPEAT_WINDOW` seconds (default 7 days, `0` turns it
off). The cycle report counts refusals (`text_refused`) and names the latest
reason, and `autonomy preflight` shows the guard and how many denylist terms are
active.

## Failing safely: circuit breaker and quarantine

An unattended entity has nobody watching for a failing dependency, so it stops
hammering one on its own. Without this, a proposal that can never succeed (its
reply parent was deleted) would be retried every cycle forever, and a systemic
failure (the PDS is down, the session was revoked, the server is rate-limiting)
would be retried at full speed every cycle.

- **Circuit breaker.** After `ATPERSON_BREAKER_THRESHOLD` consecutive *failed*
  executions (default 3) the breaker opens and the network write is held back for
  a cool-down (`ATPERSON_BREAKER_COOLDOWN`, default 15 minutes). When it elapses
  exactly one attempt is made (half-open): success closes it and resets the
  cool-down, failure re-opens it at once with the cool-down doubled, up to
  `ATPERSON_BREAKER_MAX_COOLDOWN` (default 6 hours). It heals by itself and never
  needs a human. Deciding and proposing carry on while it is open; only the
  write is held. Rate limiting is covered by the same backoff.
- **Quarantine.** A proposal that fails `ATPERSON_PROPOSAL_FAILURE_LIMIT` times
  (default 3) is moved to `<proposals>/quarantine/` with a `.reason` note, so one
  poison proposal cannot starve the ones behind it. It is not re-proposed when the
  same context decides the same words again.
- **Queue cap.** Proposing stops while `ATPERSON_SCHEDULER_MAX_PENDING`
  proposals (default 50) are already waiting, so an unauthorised or held queue
  cannot grow without bound.

Only genuine faults count. A refusal by policy, control, dry-run or the output
guard is the system working as designed and never trips the breaker.

`atperson autonomy breaker` shows the state (open until, consecutive failures,
trips, the last failure); `autonomy breaker reset` closes it by hand;
`autonomy preflight` reports it as a note, since it retries by itself; and the
cycle report includes `breaker_gate`, `breaker_trips`, `quarantined` and
`proposals_capped`. The state is one small file, `scheduler-breaker.json`, in the
data directory.

## Unattended operation: arm once, then it runs

The entity is built to act on its own after a single, deliberate setup step.
Off by default; nothing here changes what a fresh install does.

```sh
# 1. See exactly what would be authorised. Nothing is written without --apply.
atperson autonomy arm --kinds post:3/1d,reply:5/1d,like:20/1h \
    --scope moon,wolf --min-plan 0.3 --expires never

# 2. Write it (policy, envelope(s), control state), then run the preflight.
atperson autonomy arm --kinds post:3/1d,reply:5/1d,like:20/1h --scope moon,wolf --apply

# 3. Put the printed environment lines in the .env file and start the daemon.
#    ATPERSON_SCHEDULER=1  ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true  (+ credentials)

# Any time: is the whole chain in place, and what will it do?
atperson autonomy preflight        # exit 0 ready, 1 not ready
```

`--kinds` takes `kind:count/window` (a window is seconds or a number with
`s`, `m`, `h` or `d`). After `--apply` the daemon decides, freezes, and publishes
on its own, one bounded cycle at a time, with no per-action approval.

**What "no human in the loop" means.** It means no approval per action, not no
bounds. The ceilings chosen at setup are enforced on every action, forever: the
per-kind budget and spacing in the outbound policy, the same ceilings in the
standing envelope (the tighter always wins), the optional scope terms and score
floors, and duplicate suppression. Pause, dry-run, the publishing master switch
and the outbound policy still apply exactly as before, and the recovery and
fail-closed rules above are unchanged. An envelope can only narrow policy, and
the arming plan is round-tripped through the real envelope parser so it can
never be something the loader would reject or read differently.

**What arming writes.**

- the outbound policy: the requested kinds enabled with their ceilings, spacing
  (`window / count / 2`) and duplicate suppression over the window; other kinds
  are untouched;
- a standing envelope. Scope terms and score floors constrain what the entity
  *says*, so they bind `post` and `reply` only. A `like`, `repost` or `follow`
  has no text and records no decision score, so a scope or floor could never be
  satisfied and the action would silently wait for approval; those kinds go in a
  companion envelope (`<id>-actions`) bounded by their ceilings alone. By
  default the authorisation never expires (`--expires never`); pass an RFC 3339
  time to make it lapse;
- the control state: writes on, dry-run off, not paused. Per-action approval
  stays enabled as the fallback for anything an envelope does not cover.

The control state is written **last**. The envelope and policy grant nothing
until writes are on, so a failure part-way through leaves the entity inert
rather than half-armed.

**The preflight** checks every link and names what is blocking: the scheduler
switch, the publishing master switch, credentials, the control state (paused,
writes off, dry-run, offline mode), that at least one kind is enabled, and that
at least one enabled kind is covered by an unexpired envelope (using the real
coverage logic, probed the way the executor sees each kind). It prints the exact
bounds the entity will act within. A missing self DID is shown as a note: the
entity would then learn from its own posts.

**Staying in control.** Arming removes the routine approval, not your ability to
stop it: `atperson control pause` (or a remote pause record from
`ATPERSON_OPERATOR_DID`) halts publishing immediately, `atperson autonomy disarm`
turns writes off and revokes the envelopes, and `atperson outbound proposals`,
`atperson journal trace` and `atperson autonomy status` show what it has done
and is about to do.

The environment (`ATPERSON_SCHEDULER`, the publishing switch, credentials) is per
process, so `arm` prints the lines for the `.env` file instead of editing it.
