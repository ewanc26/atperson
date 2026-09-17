#include "linkage.hpp"

#include "engine.hpp"

#include <filesystem>
#include <memory>

namespace atperson {
namespace {

enum class LinkOutcome {
    NoMatch,   /* the field does not cite an executed action */
    Linked,    /* a new event was appended */
    Duplicate, /* the cited action already recorded this event */
};

/* Try one referencing field: `uri` is the at-URI the observation cites
 * (reply parent, thread root or quote target) and `via` is the field's
 * stable name. */
LinkOutcome try_link(const std::filesystem::path &journal_path, JournalContents &journal,
                     std::string_view event_uri, std::string_view author_did,
                     std::string_view uri, std::string_view via, std::string_view at) {
    if (uri.empty()) {
        return LinkOutcome::NoMatch;
    }
    const JournalAction *action = journal_find_action_by_uri(journal, uri);
    if (action == nullptr) {
        return LinkOutcome::NoMatch;
    }
    if (journal_has_event(journal, action->id, event_uri)) {
        return LinkOutcome::Duplicate;
    }

    JournalEvent event;
    event.action_id = action->id;
    event.event_uri = std::string(event_uri);
    event.author_did = std::string(author_did);
    event.via = std::string(via);
    event.at = std::string(at);
    append_journal_event(journal_path, event);
    journal.events.push_back(std::move(event));
    return LinkOutcome::Linked;
}

} // namespace

LinkageResult link_observation(const std::filesystem::path &journal_path,
                               JournalContents &journal, std::string_view event_uri,
                               std::string_view author_did, const ConversationContext &context,
                               std::string_view at) {
    LinkageResult result;
    /* Order is deliberate: a direct reply's parent is the strongest signal,
     * then the thread root, then a quote. An observation that references the
     * same action twice (e.g. a reply whose parent and root are both the
     * action — a self-reply thread) links once, via the first field. A
     * duplicate on any field ends the walk: the event is already recorded,
     * so weaker fields must not re-count it. */
    const LinkOutcome parent =
        try_link(journal_path, journal, event_uri, author_did, context.reply_parent_uri, "parent", at);
    if (parent == LinkOutcome::Linked) {
        result.linked++;
        return result;
    }
    if (parent == LinkOutcome::Duplicate) {
        result.duplicates++;
        return result;
    }
    const LinkOutcome root =
        try_link(journal_path, journal, event_uri, author_did, context.reply_root_uri, "root", at);
    if (root == LinkOutcome::Linked) {
        result.linked++;
        return result;
    }
    if (root == LinkOutcome::Duplicate) {
        result.duplicates++;
        return result;
    }
    if (try_link(journal_path, journal, event_uri, author_did, context.quote_uri, "quote", at) ==
        LinkOutcome::Linked) {
        result.linked++;
    }
    return result;
}

SyncLinker make_journal_linker(const std::filesystem::path &journal_path) {
    if (!std::filesystem::exists(journal_path)) {
        return nullptr;
    }
    auto journal = std::make_shared<JournalContents>(load_journal(journal_path));
    return [journal_path, journal](const SyncObservation &observation) {
        link_observation(journal_path, *journal, observation.source_uri, observation.author_did,
                         observation.context, observation.created_at);
    };
}

} // namespace atperson
