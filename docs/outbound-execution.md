# Outbound execution

`atperson` can publish an operator-approved AT Protocol post or reply through Wolfram. It cannot reach this path as a side effect of learning, memory or planning: the operator has to provide a frozen action document and explicitly run `publish`.

The main rule is the same as the policy layer: **a learned plan is evidence, not permission**.

## Gate order

```sh
atperson publish <action-file>
```

The gates run in this order, and a later one is never reached until the earlier ones pass:

1. **Pause** — an operator pause stops the action immediately.
2. **Outbound policy** — kind enablement, rolling budget, minimum interval and duplicate suppression are evaluated.
3. **Dry run** — with dry-run enabled, the fully evaluated action is reported and stopped before login.
4. **Control approval** — writes must be enabled and, when approval is required, the exact action digest must be present in the approval set.
5. **Network write** — only here is a Wolfram session created and the frozen record submitted.

A refusal or dry run does not read the AT Protocol credentials and does not establish a session.

## Frozen action document

Execution takes an `atperson-outbound-action` v1 document:

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

`post`, `reply`, `like`, `repost` and `follow` are executable. `unfollow` and `moderation` are refused at parse time rather than being downgraded to something else. A `like` or `repost` carries a `subject` (the subject record's AT URI) and a `follow` carries a `subject` (the subject DID); all three must have no `text`. A `post` or `reply` carries `text` and must not carry a `subject`. Each kind is its own default-off policy entry with its own rate budget, so enabling posts never enables likes.

The digest is the 16-character lowercase hexadecimal control digest for the exact inspected action. Replies carry stable root and parent AT URIs, and likes and reposts a subject AT URI; the current CIDs are resolved from the live records at execution time and used as `com.atproto.repo.strongRef`s. The text itself is never regenerated during execution.

## Reviewing scheduler proposals

When the autonomous scheduler is enabled it only ever *freezes* proposals: each is written as `<digest>.json` under `ATPERSON_SCHEDULER_PROPOSALS` (default `<data>/scheduler/proposals`) and waits there for an operator approval or a standing authorization envelope. To see what is waiting, without reading the directory by hand:

```sh
atperson outbound proposals [digest]
```

For every pending proposal it prints the digest, kind, record key and creation time, the text (previewed in the list, complete when a digest is given) or the reply/subject targets, any recorded decision evidence, and two verdicts computed for that exact frozen action: the outbound **policy** answer (allow, deny or defer, with the stable reason code) and the **control gate** answer (paused, writes off, dry-run, or digest not yet approved). Unreadable files are reported by name.

The command is strictly read-only: it approves nothing, consumes no budget, removes no proposal and opens no session. Approving a digest stays an explicit `atperson control approve <digest>`. A standing authorization envelope can also cover a proposal, and that is evaluated only at execution time, so the control-gate line reflects per-digest approval only.

## Idempotency

The record key is frozen in the action document and the write uses `com.atproto.repo.putRecord`. Re-running the same document therefore targets the same record instead of creating a second post after an ambiguous failure.

The rate budget is committed only after confirmed success. Failed writes leave it untouched so the operator can retry the same frozen action.

## Audit and experience history

Every attempt — executed, dry-run, denied, deferred or failed — is appended to the outbound audit log. `ATPERSON_OUTBOUND_AUDIT` defaults to `<data>/outbound-audit.jsonl`.

The audit entry includes the time, kind, rkey, digest, outcome, stable reason, optional detail and the resulting URI/CID when one exists. Credentials never enter the log.

The separate [`action-journal.md`](action-journal.md) records the entity's own outbound experience for replay and later explicit valence mapping. The audit log remains an operator-facing execution record; neither file is a substitute for the other.

## Locking

`publish` uses a dedicated `.outbound-lock` in the data directory. It does not take the daemon's long-held `.writer-lock`, so ingestion and operator-led publishing do not block one another unnecessarily. The outbound lock serialises the budget read/modify/write path and prevents concurrent publishes from losing an update.

## Exit behaviour

`publish` reports the outcome, reason, detail and whether a budget entry was committed.

- exit `0`: executed, dry-run, denied or deferred — these are completed decisions;
- exit `1`: execution failure;
- exit `2`: invalid or missing CLI arguments.

## Offline coverage

`tests/outbound/execute.cpp` covers gate ordering, fail-closed behaviour, budget-on-success, stable rkeys, reply CID resolution and audit appends using a fake `OutboundWriter`. CI never publishes records. The Wolfram adapter in `src/app/atproto/writer.cpp` is the only implementation allowed to perform the protocol write.