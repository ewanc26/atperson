#include "intent/mutate.hpp"

#include "intent/state.hpp"
#include "state/time.hpp"

#include <cstdint>
#include <string>

namespace atperson {

IntentMutation record_pending_intent(const std::filesystem::path &journal_path,
                                     const OutboundAction &action,
                                     const OutboundExecutionResult &result,
                                     const IntentConfig &config, std::int64_t now,
                                     std::string_view now_rfc3339) {
    if (!config.enabled) {
        return IntentMutation::NotTracked;
    }
    if (result.outcome != OutboundExecutionOutcome::Executed) {
        return IntentMutation::NotTracked;
    }

    /* The thread root is what a conversation is keyed on: a reply's root
     * pre-exists in the frozen document; an original post's root is its own
     * just-written record URI. */
    std::string root;
    if (action.kind == OutboundActionKind::Reply) {
        root = action.reply_root;
    } else if (action.kind == OutboundActionKind::Post) {
        root = result.written.uri;
    }
    if (root.empty()) {
        return IntentMutation::NotTracked;
    }

    const JournalContents journal = load_journal(journal_path);
    const JournalIntent *existing = latest_intent(journal, root);
    const bool can_continue = existing != nullptr &&
                              derive_intent_state(*existing, now) == IntentState::Open;

    if (can_continue) {
        for (const std::string &recorded : existing->actions) {
            if (recorded == action.rkey) {
                return IntentMutation::Duplicate;
            }
        }
        JournalIntent continuation = *existing;
        continuation.actions.push_back(action.rkey);
        continuation.state = IntentState::Open;
        continuation.at_epoch = now > 0 ? static_cast<std::uint64_t>(now) : 0u;
        continuation.at = std::string(now_rfc3339);
        append_journal_intent(journal_path, continuation);
        return IntentMutation::Continued;
    }

    if (active_intent_count(journal, now) >= config.max_active) {
        return IntentMutation::CapReached;
    }

    const std::int64_t expires = now + config.window_seconds;
    JournalIntent intent;
    intent.id = root;
    intent.actions.push_back(action.rkey);
    intent.responder = "anyone";
    intent.expires_at_epoch = expires > 0 ? static_cast<std::uint64_t>(expires) : 0u;
    intent.expires_at = rfc3339_from_unix(expires);
    intent.max_continuations = config.max_continuations;
    intent.state = IntentState::Open;
    intent.at_epoch = now > 0 ? static_cast<std::uint64_t>(now) : 0u;
    intent.at = std::string(now_rfc3339);
    append_journal_intent(journal_path, intent);
    return IntentMutation::Opened;
}

} // namespace atperson