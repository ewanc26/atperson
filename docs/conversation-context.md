# Conversation and reply context

`atperson` records the conversational position of every observation — reply root/parent and quote target — as first-class planning metadata. This is the issue #24 state model: thread structure is preserved without being learned, so a future reply planner knows which conversation a post belongs to while the learning core never trains on URIs.

## What is recorded

Each mirrored ledger entry carries an `atp_conversation_context`:

| Field | Meaning | Empty when |
|-------|---------|------------|
| `reply_root_uri` | Thread root the post replies to | Top-level post, or root unresolvable |
| `reply_parent_uri` | Direct parent of the reply | Top-level post, or parent deleted before fetch |
| `quote_uri` | Record the post quotes | Not a quote |

All three are stable AT-URIs (`at://did/collection/rkey`) — identifiers, not content. The quoted post's text is never merged into learnable text: a quote embed is excluded from the text extraction path entirely, so the entity's vocabulary can only grow from text its author actually wrote.

## Where it lives

- **Sync layer** (`src/app/atproto/extract.cpp`): `extract_feed_item` reads `item.reply.root.uri`, `item.reply.parent.uri` and the record's `app.bsky.embed.record` embed, translating one `feedViewPost` into a `SyncObservation` with `.context`. Pure cJSON-in/observation-out — offline testable, no Wolfram, no network.
- **Policy** (`src/app/ingestion/policy.cpp`): a quote embed sets `is_quote` and never provides text fallback; precedence is repost > reply > quote > eligible. An empty-text quote is honestly `EmptyText` (nothing of the author's own to learn), not `NonTextOnly` (which means media-only).
- **Core mirror** (`src/core/graph/lifecycle.c`): `atp_graph_add_ledger_entry_with_context` stores the context in a parallel `ledger_contexts` array; `atp_graph_ledger_context` reads it back. Plain `atp_graph_add_ledger_entry` records with empty context.
- **Ledger** (`src/core/ledger/`): ledger format v3 appends the three context URIs to every entry record, so context is durable authority — not just a snapshot mirror. `atp_ledger_append_with_context` writes it; `atp_ledger_entry_context` reads it by id; `atp_ledger_append` (and the C++ `Ledger::append` overload without a context) writes empty context. Legacy v1/v2 logs migrate on open with empty context, the honest value for records that predate capture. `atp_replay_ledger` reads context per entry and mirrors it through `atp_graph_add_ledger_entry_with_context`, so a replay rebuild restores reply/quote continuity exactly as a snapshot restore does.
- **C++ surface** (`include/atperson/graph.hpp`): `LanguageGraph::record_ledger_entry(entry, context)` and `ledger_context(index)`. `ConversationContext` (std::string URIs) converts to the fixed-size C struct at the boundary; URIs longer than `ATPERSON_CONTEXT_URI_BYTES - 1` throw. `Ledger::append(..., context, ...)` and `Ledger::entry_context(id)` expose the same at the runtime layer.

## Persistence

Two durable representations carry context, and both are rebuilt from the ledger:

- **Ledger** (authority): each entry record ends with `root_len | reply_root_uri`, `parent_len | reply_parent_uri`, `quote_len | quote_uri` (u32 lengths, empty = absent). Context is covered by the record CRC but is outside the content digest — it is metadata, not learnable bytes. Replay rebuilds it.
- **Snapshot mirror** (portable state): snapshot section `ATP_SECTION_CONTEXT` (tag 10) writes one row per mirrored entry, in mirror order — the row index is always the ledger mirror index, no side table. Each row is three optional strings (root, parent, quote).

The snapshot section is optional and load-time validated:

- Absent section (pre-#24 snapshot): loads with empty context everywhere. No migration needed.
- Row count greater than the ledger mirror: rejected as format corruption.
- The `LEDGER` section must be decoded first; a stream that reorders them is not a snapshot this core produced.

## Invariants

- Context is planning metadata, never learnable content. No URI is tokenized, embedded, or scored.
- Malformed context (missing `reply` member, non-object, missing `uri` fields) yields empty strings — the observation is never dropped for malformed context.
- A deleted or missing parent still yields its identifiers: the strongRef survives in the feed even when the record is gone, which is exactly what audit needs.
- Empty-text quotes are skipped visibly (`EmptyText` in the ledger) with the quote URI retained in context.
- Self-authored replies are skipped from learning but their context is still mirrored — the ledger sees the entity's own conversational behaviour for audit.

## API

```c
#include "atperson/core.h"

atp_conversation_context context = {0};
strcpy(context.reply_root_uri, "at://did:plc:root/app.bsky.feed.post/3k1");
strcpy(context.reply_parent_uri, "at://did:plc:parent/app.bsky.feed.post/3k2");

/* Durable ledger entry carrying context (issue #49). */
uint64_t id = 0u;
atp_status status = ATP_OK;
atp_ledger_append_with_context(ledger, source_uri, author_did, observed_at, digest,
                               ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, text,
                               text_len, &context, &id, &status);

/* Graph mirror (also restored by atp_replay_ledger). */
atp_graph_add_ledger_entry_with_context(graph, &entry, &context);

atp_conversation_context read_back = {0};
atp_ledger_entry_context(ledger, id, &read_back);
atp_graph_ledger_context(graph, 0u, &read_back);
```

`ATPERSON_CONTEXT_URI_BYTES` is 256: enough for any at:// URI (DID ~60 chars, NSID ≤317 by spec, rkey ≤512 in practice, but real post URIs are well under 200). A URI at or beyond the bound is rejected with `ATP_ERR_INVALID_ARGUMENT` before any mutation.
