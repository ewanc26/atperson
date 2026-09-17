# Outbound execution

The runtime can now perform an **operator-approved** AT Protocol post or reply
through Wolfram. This is deliberately narrow: execution is a gated, audited path
led by an explicit operator command, never a side effect of learning, memory or
planning.

An accepted core plan is evidence, never permission. The outbound layer is the
only path from a decision to a network write, and it is fail-closed at every
step.

## Gate order

`atperson publish <action-file>` runs the gates in this exact order. A later
gate is never reached until every earlier one passes, and any refusal returns
without touching the network or the rate budget.

1. **Pause** — `control pause` refuses the action before anything is evaluated.
2. **Outbound policy (#23)** — per-kind enable/disable, rolling rate budget,
   minimum interval and duplicate suppression, evaluated by
   `evaluate_outbound_policy`. `deny` and `defer` are reported in the same
   inspectable reason vocabulary as `atperson outbound`.
3. **Dry-run** — if `control dry-run on`, the action is fully evaluated and
   reported as `dry_run`, then stopped. No session is established.
4. **Control gate (#22)** — `ensure_outbound_allowed` requires
   `writes_enabled=true` and, when `approval_required=true`, the action's exact
   digest in `approved_digests`.
5. **Network write** — only now is a Wolfram session created and the record
   submitted.

A dry run or refusal never reads `ATPERSON_IDENTIFIER`/`ATPERSON_APP_PASSWORD`
and never logs in. Credentials are construction arguments to the session and are
never logged, persisted or written to the audit log.

## Action document

Execution is driven by a frozen `atperson-outbound-action` v1 JSON document:

```json
{
  "format": "atperson-outbound-action",
  "version": 1,
  "kind": "reply",
  "text": "the exact text the operator inspected",
  "rkey": "3kabc",
  "created_at": "2026-09-17T00:00:00Z",
  "digest": "0123456789abcdef",
  "reply": {
    "root": "at://did:plc:…/app.bsky.feed.post/…",
    "parent": "at://did:plc:…/app.bsky.feed.post/…"
  }
}
```

- `kind` is `post` or `reply` only. Other #23 kinds are rejected, not silently
  downgraded.
- `digest` is the **#22 control digest**: exactly 16 lowercase hex characters.
  A malformed digest is rejected at parse time because it could never match an
  approval.
- `reply` carries the parent and thread-root **at-URIs** from #24. Their content
  CIDs are resolved from the live records at execution time and become
  `com.atproto.repo.strongRef`s. The entity never regenerates the text.

## Idempotency

The record key (`rkey`) is frozen in the document and the write uses
`com.atproto.repo.putRecord`, not `createRecord`. Re-submitting the same
document replaces the same record with byte-identical content, so a retry after
an ambiguous failure cannot create a duplicate post.

The rate budget is recorded **only on a confirmed success**. A write that fails
(including an ambiguous network failure) leaves the budget untouched, so the
operator can retry the frozen action rather than silently losing it.

## Audit log

Every attempt — executed, dry run, refused or failed — is appended as one JSON
object to the outbound audit log (`ATPERSON_OUTBOUND_AUDIT`, default
`<data>/outbound-audit.jsonl`). Entries carry the timestamp, kind, rkey, digest,
outcome, stable reason code, optional detail and the resulting URI/CID. The log
is credential-free and append-only, and is **not** learned state; feeding
outcomes back into learning is a separate concern (#27).

## Locking

`publish` takes a dedicated `.outbound-lock` inside the data directory. It never
takes the daemon's `.writer-lock`, so publishing does not wait for ingestion and
vice versa. The lock serialises the budget read-modify-write so two concurrent
publishes cannot lose an update.

## CLI

```sh
atperson publish <action-file>
```

Output reports the outcome, reason, detail and whether budget was recorded, then
points at the audit log. Exit status is `0` for executed, dry-run, denied and
deferred outcomes (these are decisions, not errors) and `1` for an execution
failure. Unknown/missing arguments return `2`.

## Offline coverage

The ordering, fail-closed behaviour, budget-on-success, idempotent rkey, reply
CID resolution and audit append are covered by `tests/outbound/execute.cpp`
against a fake `OutboundWriter`; the network is never touched in tests, and CI
does not publish records. The Wolfram-backed adapter
(`src/app/atproto/writer.cpp`) is the only place that calls
`wf_agent_put_record_typed` / `wf_agent_get_record_typed`.
