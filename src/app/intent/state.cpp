#include "intent/state.hpp"

#include <algorithm>
#include <cstdint>

namespace atperson {

IntentState derive_intent_state(const JournalIntent &latest, std::int64_t now) {
    if (latest.state != IntentState::Open) {
        /* Terminal states were journaled by the sweep; they never reopen. */
        return latest.state;
    }
    const std::int64_t expires = static_cast<std::int64_t>(latest.expires_at_epoch);
    if (now > expires) {
        return IntentState::Expired;
    }
    /* Continuations used == actions.size() - 1; reaching the budget closes
     * the conversation even inside its window (the "don't orbit one thread
     * forever" bound). */
    if (latest.actions.size() - 1u >= latest.max_continuations) {
        return IntentState::Closed;
    }
    return IntentState::Open;
}

const JournalIntent *latest_intent(const JournalContents &journal, std::string_view id) {
    const JournalIntent *latest = nullptr;
    for (const JournalIntent &entry : journal.intents) {
        if (entry.id == id) {
            latest = &entry;
        }
    }
    return latest;
}

std::vector<const JournalIntent *> active_intents(const JournalContents &journal,
                                                  std::int64_t now) {
    /* The current record of a conversation is its last entry, terminal or
     * not: the sweep appends an Expired/Closed line that supersedes every
     * earlier Open line for that thread, and that must hide it even though
     * the earlier entry still derives Open by itself. */
    std::vector<const JournalIntent *> latest;
    for (const JournalIntent &entry : journal.intents) {
        bool replaced = false;
        for (const JournalIntent *&slot : latest) {
            if (slot->id == entry.id) {
                slot = &entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            latest.push_back(&entry);
        }
    }
    std::vector<const JournalIntent *> active;
    for (const JournalIntent *entry : latest) {
        if (derive_intent_state(*entry, now) != IntentState::Open) {
            continue;
        }
        active.push_back(entry);
    }
    return active;
}

std::size_t active_intent_count(const JournalContents &journal, std::int64_t now) {
    return active_intents(journal, now).size();
}

const JournalIntent *continuation_intent(const JournalContents &journal,
                                         std::string_view source_id,
                                         std::string_view author_did, std::int64_t now) {
    if (source_id.empty()) {
        return nullptr;
    }
    const std::vector<const JournalIntent *> active = active_intents(journal, now);
    for (const JournalEvent &event : journal.events) {
        if (event.event_uri != source_id) {
            continue;
        }
        if (!author_did.empty() && event.author_did != author_did) {
            continue;
        }
        for (const JournalIntent *intent : active) {
            bool action_in_conversation = false;
            for (const std::string &action : intent->actions) {
                if (action == event.action_id) {
                    action_in_conversation = true;
                    break;
                }
            }
            if (!action_in_conversation) {
                continue;
            }
            if (intent->responder != "anyone" && intent->responder != event.author_did) {
                continue;
            }
            return intent;
        }
    }
    return nullptr;
}

} // namespace atperson