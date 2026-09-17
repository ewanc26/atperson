# Conversation and reply context

`atperson` preserves where an observation sits in a conversation without treating AT URIs as learnable text. Reply roots, direct parents and quote targets are planning metadata: useful for continuity, but never tokenised into the language graph.

## Stored fields

Each ledger/mirror entry can carry an `atp_conversation_context`:

| Field | Meaning | Empty when |
| --- | --- | --- |
| `reply_root_uri` | Thread root | Top-level post or root unavailable |
| `reply_parent_uri` | Direct reply parent | Top-level post or parent unavailable |
| `quote_uri` | Quoted record | Not a quote |

These are stable AT URIs, not content. Quoted text is not merged into the author's learnable text; a quote embed contributes structure only.

## Data flow

The same context travels through the existing boundaries:

- `src/app/atproto/extract.cpp` extracts reply/quote identifiers into `SyncObservation`;
- ingestion policy decides whether the author's own text is eligible to learn;
- the C23 ledger stores context durably beside the observation;
- the graph snapshot mirrors it for local inspection;
- replay restores the same context from the ledger;
- the C++ wrapper converts between string-owning runtime values and the fixed C representation.

Malformed or missing context fields degrade to empty identifiers instead of dropping an otherwise valid observation.

## Persistence

The ledger is authoritative. Format v3 stores the three optional context URIs on each entry, covered by the record CRC but excluded from the learnable content digest.

The portable snapshot also mirrors context in `ATP_SECTION_CONTEXT` (tag 10). Older snapshots without the section load with empty context, and legacy v1/v2 ledgers migrate with empty context because those formats never captured it.

Replay reads context from the ledger and reconstructs the snapshot mirror. Context is therefore not a snapshot-only convenience.

## Invariants

- Conversation identifiers are metadata, never vocabulary.
- Quote text is not learned as though the observing author wrote it.
- Missing/deleted parents do not invalidate the identifiers that were captured.
- Empty-text quotes can remain visible in the ledger as skipped observations with their quote URI retained.
- Self-authored records may be excluded from learning while their conversational metadata remains auditable.

## API

```c
#include "atperson/core.h"

atp_conversation_context context = {0};
strcpy(context.reply_root_uri, "at://did:plc:root/app.bsky.feed.post/3k1");
strcpy(context.reply_parent_uri, "at://did:plc:parent/app.bsky.feed.post/3k2");

uint64_t id = 0u;
atp_status status = ATP_OK;
atp_ledger_append_with_context(
    ledger,
    source_uri,
    author_did,
    observed_at,
    digest,
    ATPERSON_SCHEMA_VERSION,
    ATP_LEDGER_OUTCOME_PENDING,
    text,
    text_len,
    &context,
    &id,
    &status);

atp_graph_add_ledger_entry_with_context(graph, &entry, &context);

atp_conversation_context read_back = {0};
atp_ledger_entry_context(ledger, id, &read_back);
atp_graph_ledger_context(graph, 0u, &read_back);
```

`ATPERSON_CONTEXT_URI_BYTES` is 256 bytes per stored URI. Inputs that do not fit are rejected before mutation rather than truncated into a different identifier.