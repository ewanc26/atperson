# Action and outcome journal

The observation ledger records what the entity observed. The action journal records what the entity itself attempted and what happened afterwards.

Keeping those streams separate matters. Third-party observations and self-authored actions have different provenance and different replay semantics, but both need to remain durable and inspectable.

## What the journal records

The append-only journal lives at `<data>/action-journal.jsonl`, or `ATPERSON_ACTION_JOURNAL` when overridden. Every line is one compact JSON object and each append is flushed durably.

There are five entry kinds.

### `action`

An outbound attempt. The entry keeps the frozen rkey, exact text, approval digest, execution outcome and, after a confirmed write, the resulting AT URI and CID.

The action id is the frozen rkey. It is stable before execution, survives idempotent `putRecord` retries and matches the tail of an executed record's URI.

An autonomous decision may also record its *prediction* of the outcome, derived deterministically at execution time from the frozen action document's decision evidence:

```json
"expectation": {
  "kind": "approach",
  "reply_likelihood": 0.7,
  "tokens": ["the", "moon", "loyal", "companion"]
}
```

`kind` is one of `action`, `interaction` or `approach`; `reply_likelihood` is the judged chance the record draws a reply, clamped to [0, 1]; `tokens` are the exact text's distinct tokens in first-appearance order, capped at 8. Operator-authored actions carry no decision evidence and record no expectation.

### `event`

A later public record that references an executed action. `via` records whether the reference came through `parent`, `root` or `quote`. Events link to the stable action id rather than trying to match mutable text.

### `valence`

An explicit experience-derived state update. It records the token, kind, signal, source and optional provenance used to create the update. Hand-applied events have no mapping provenance; `journal map` uses `map:<rule-id>`.

The journal stores the evidence. The C23 valence API applies it, and `rebuild` replays it.

### `resolution`

The recorded lifetime state of one action's expectation, written by the expectation-resolution pass. Only terminal states are ever written: `met` (a linked event landed before the window closed), `unmet` (contact happened, but not in time) or `expired` (the window closed with no contact). A pending expectation has no line.

## Audit log versus journal

The outbound audit log and action journal deliberately overlap because they answer different questions:

- the **audit log** is the operator-facing execution history;
- the **journal** is durable experience provenance used by events, valence mapping and rebuild.

Neither is reconstructed from the other.

## CLI

```sh
atperson journal actions [limit]
atperson journal events [limit]
atperson journal valence [limit]
atperson journal resolutions [limit]
atperson journal intents [limit]
atperson journal apply <token> <kind> <signal> <source-id>
atperson journal map <rule-file>
atperson journal resolve
```

The listing commands are read-only. `apply`, `map` and `resolve` mutate persistence and therefore take the data-directory writer lock. `apply` and `map` additionally touch learned state; `resolve` touches only the journal.

## Applying one valence event

`journal apply` is the explicit single-event path. The operator supplies the token, valence kind, signal and source id. The token must already exist in learned vocabulary; an outcome is not allowed to create vocabulary merely because an operator attached a value to it.

The graph update, journal append and model save happen under the same writer lock. A bad argument returns exit code 2; persistence failures abort rather than leaving the three durable effects half-committed.

## Mapping journal outcomes

`journal map` is the batch form. The operator owns a rule table describing how recorded outcomes should map to valence:

```json
{
  "format": "atperson-valence-rules",
  "version": 1,
  "rules": [
    {
      "id": "denied-negative",
      "when": {"outcome": "denied"},
      "kind": "action",
      "signal": -0.5
    },
    {
      "id": "replied-positive",
      "when": {
        "outcome": "executed",
        "min_events": 1,
        "within_seconds": 86400
      },
      "kind": "interaction",
      "signal": 0.5
    }
  ]
}
```

Rules run in document order and the first match wins for an action. A rule may match an execution outcome and optionally require a number of later linked events within a time window. An action that matches no rule produces no valence update.

For a matched action, the signal is applied to each distinct token in the action text that already exists in the vocabulary. Unknown tokens are skipped rather than interned.

Mapping is idempotent. A derived entry is identified by source, mapping provenance and token; running the same mapping again does not apply it twice.

Malformed format/version data, duplicate rule ids, unknown outcome or valence kinds and out-of-range signals fail before learned state is touched.

## Expectation resolution

An executed action's recorded prediction is judged against the events that later referenced it by the resolution pass (`journal resolve`, or automatically at the start of every enabled scheduler cycle). The judged state is a pure function of the journal and the current clock:

- **met** — at least one linked event landed within the expectation window (one week after the attempt; a reply exactly on the boundary is on time);
- **unmet** — a linked event exists but none landed in time (a late reference);
- **expired** — the window closed with no linked event;
- **pending** — no event yet and the window still open.

Only terminal states are written. Pending expectations never get a line, and replaying the pass never duplicates an already-recorded terminal state. When a recorded state later changes — an expired action gains a late reference, say — an additional resolution line records the knowledge change.

Because each attempt instant is normalised to its Unix epoch, an idle author or a reply that simply cannot be timestamped never counts as an on-time reply.

An action whose conversation is memory of an open intent uses that intent's reply window instead: the event must land inside the intent window to resolve `met`, and an open intent whose window closes with no contact resolves `unmet` even though the week-long expectation window may still be open (#150).

The scheduler runs the resolution pass before any new decision, so a cycle always first resolves the events that landed since the last cycle. The pass is journal-only and idempotent, so it is safe at the top of every enabled cycle.

A rule table may also condition on the resolved state:

```json
{
  "id": "expected-but-unmet",
  "when": {"outcome": "executed", "expectation": "unmet"},
  "kind": "approach",
  "signal": -0.2
}
```

Only the three terminal states (`met`, `unmet`, `expired`) are authorable conditions. A condition never fires for an action without a prediction, and never while the expectation is still pending.

### `intent`

The recorded pending-intent state of an autonomous conversation, written by the scheduler's post-execution intent hook (#150) and read back by the next cycle. `intent` requires journal format 4. Each entry records the conversation thread URI, the executed action ids that make it up, the invited respondent, the reply-window end, the continuation budget and the current life-cycle state:

```json
{
  "type": "intent",
  "format": 4,
  "id": "at://did:plc:self/app.bsky.feed.post/<rkey>",
  "actions": ["<rkey1>"],
  "responder": "anyone",
  "expires_at_epoch": 1721102400,
  "max_continuations": 3,
  "state": "open",
  "at_epoch": 1719878400,
  "at": "2024-07-01T16:00:00Z"
}
```

State is one of `open`, `expired` or `closed`:

- **open** — the conversation is waiting on the reply window;
- **expired** — the start-of-cycle sweep found `now` past `expires_at_epoch` and journaled the terminal state;
- **closed** — the continuation budget was reached (`actions.size() - 1` past `max_continuations`); a conversation never orbits one thread forever.

The current record of a conversation is the last entry with its id, terminal or not — the sweep's terminal line supersedes every earlier `open` line for that thread. Only the latest `open` entry can drive a continuation, so a swept conversation never fires again. The hook writes `open` entries when an executed post/reply starts a conversation, or the id/`actions` of an already-open conversation when a continuation reply was executed. An idempotence check runs before every write: a conversation at its budget (`open` entry whose `actions` already fill the cap) is never appended to again.

Every intent entry is validated on append and load: non-empty actions, `responder` of `anyone` or a DID, positive `at_epoch`/`expires_at_epoch`, `max_continuations` in [1, 64], a known state and a valid timestamp.

The resolution pass uses intents separately from `expectation`: when an executed action belongs to an open intent, a linked event is only judged `met` if it landed inside the intent's own reply window; if an open intent's window closes with no contact, the resolution pass flips the prediction `unmet`.

Intent recording is a scheduler concern and all of it is off by default (`ATPERSON_INTENTS=1`). Operator-authored actions never record intents.

## Replay

`atperson rebuild` replays two evidence streams in a fixed order:

1. observation-ledger entries in ledger id order;
2. explicit journal valence entries in journal append order.

That makes experience-derived valence reconstructable rather than snapshot-only state. The ledger remains the authority for third-party observations, while the journal is the authority for the entity's own recorded experience.

## Journal-integrity MAC

`publish` may attach a keyed integrity check to an executed action. This is a symmetric journal-integrity MAC, not a badge.blue record attestation and not a signing key: it proves the journal entry was written by someone holding the key, and lets an operator detect tampered journal entries by re-deriving the MAC.

When `ATPERSON_JOURNAL_MAC_KEY` is set, `publish` computes the MAC over a canonical string

```text
atperson-journal-mac-v1:<repo-did>:<rkey>:<sha256-hex of exact text>:<created-at>
```

and stores it in the journal entry's `mac` field:

```json
"mac": {
  "mode": "hmac-sha256",
  "key_hint": "<first 8 hex chars of the key>",
  "digest": "<sha256-hex of the canonical string>",
  "sig": "<hmac-sha256 hex of the canonical string>"
}
```

`key_hint` is a prefix for identifying which key produced an entry; it is not a key and never a fabricated DID. Entries written without the MAC, including every v1 entry, load as a null `mac` field.

Verification is standalone: given the entry and the key, the digest mismatch, signature mismatch, missing-key and absent-`mac` failure modes are distinct. A missing key never makes an authenticated entry look unauthenticated; it is reported as its own reason code.

## Crash and format handling

- A missing journal means an empty journal.
- A torn final line is reported and truncated during load.
- Unsupported versions, malformed entries and impossible field combinations fail explicitly.
- Unknown entry types are not skipped.
- The current journal format version is 4. v1 action entries and v2 action/event/valence entries load unchanged (absent expectations are exactly nullopt); a v2 resolution line is refused as foreign schema, and an `intent` line inside an older (`< 4`) file is refused as foreign schema exactly like it.

The file format is append-only and the caller owns cross-process serialisation through the appropriate runtime lock.

## Network boundary

The journal performs no network I/O. `publish` writes through Wolfram and records the result; the journal itself only stores experience and explicit valence provenance. Autonomous posting, replies, likes, follows, reposts, DMs and moderation are not created as side effects of journal replay or learning.