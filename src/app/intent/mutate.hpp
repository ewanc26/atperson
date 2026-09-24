#ifndef ATPERSON_INTENT_MUTATE_HPP
#define ATPERSON_INTENT_MUTATE_HPP

// Pending social intent recording (#150): append the journal `intent` entry
// for an executed action — opening a new conversation, or continuing one the
// entity is already holding.
//
// Deterministic rules, all derived from the frozen action document and the
// journal (never from the graph):
//   * only an executed public post or reply is tracked (no thread => nothing);
//   * a reply continues the open intent whose thread root equals the reply's
//     root; otherwise a fresh intent is opened keyed to that root;
//   * a post is its own thread: a fresh intent keyed to the written record
//     URI;
//   * a fresh intent is opened only while the active-intent count is below
//     the cap — the cap refuses (fail-closed), it never silently drops the
//     oldest intent, which expires explicitly on its own sweep;
//   * the exact action id is recorded once per conversation: a continuation
//     that is already in the intent's `actions` writes nothing (idempotent).
//
// The caller owns locking and has already executed the action; this function
// never refuses an action, only whether a conversation is tracked.
//
// Failure modes: journal I/O throws std::runtime_error; a corrupt journal
// throws JournalError through load_journal.

#include "intent/config.hpp"
#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/execute.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace atperson {

enum class IntentMutation {
    NotTracked, /* feature disabled, non-executed outcome, or no thread */
    Opened,     /* a fresh intent was journaled for a new conversation */
    Continued,  /* an existing open intent gained the executed action */
    Duplicate,  /* the action was already recorded in this conversation */
    CapReached, /* a fresh intent was refused: active-intent cap reached */
};

/* Record the executed action's conversation state. `now` stamps the entry;
 * `now_rfc3339` is the caller's pre-formatted instant. Returns what was
 * decided (or refused). */
[[nodiscard]] IntentMutation record_pending_intent(const std::filesystem::path &journal_path,
                                                   const OutboundAction &action,
                                                   const OutboundExecutionResult &result,
                                                   const IntentConfig &config, std::int64_t now,
                                                   std::string_view now_rfc3339);

} // namespace atperson

#endif