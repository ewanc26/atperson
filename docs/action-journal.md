# Action and outcome journal

The observation ledger records what the entity observed. The action journal records what the entity itself attempted and what happened afterwards.

Keeping those streams separate matters. Third-party observations and self-authored actions have different provenance and different replay semantics, but both need to remain durable and inspectable.

## What the journal records

The append-only journal lives at `<data>/action-journal.jsonl`, or `ATPERSON_ACTION_JOURNAL` when overridden. Every line is one compact JSON object and each append is flushed durably.

There are three entry kinds.

### `action`

An outbound attempt. The entry keeps the frozen rkey, exact text, approval digest, execution outcome and, after a confirmed write, the resulting AT URI and CID.

The action id is the frozen rkey. It is stable before execution, survives idempotent `putRecord` retries and matches the tail of an executed record's URI.

### `event`

A later public record that references an executed action. `via` records whether the reference came through `parent`, `root` or `quote`. Events link to the stable action id rather than trying to match mutable text.

### `valence`

An explicit experience-derived state update. It records the token, kind, signal, source and optional provenance used to create the update. Hand-applied events have no mapping provenance; `journal map` uses `map:<rule-id>`.

The journal stores the evidence. The C23 valence API applies it, and `rebuild` replays it.

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
atperson journal apply <token> <kind> <signal> <source-id>
atperson journal map <rule-file>
```

The listing commands are read-only. `apply` and `map` mutate learned state and therefore take the data-directory writer lock.

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

The file format is append-only and the caller owns cross-process serialisation through the appropriate runtime lock.

## Network boundary

The journal performs no network I/O. `publish` writes through Wolfram and records the result; the journal itself only stores experience and explicit valence provenance. Autonomous posting, replies, likes, follows, reposts, DMs and moderation are not created as side effects of journal replay or learning.