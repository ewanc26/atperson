#include "engagement.hpp"

#include "protocol.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace atperson {
namespace {

/* "at://<authority>/..." -> authority, or empty. */
std::string at_uri_authority(std::string_view uri) {
    constexpr std::string_view scheme = "at://";
    if (!uri.starts_with(scheme)) {
        return {};
    }
    const std::string_view rest = uri.substr(scheme.size());
    return std::string(rest.substr(0, rest.find('/')));
}

} // namespace

const char *engagement_mode_name(EngagementMode mode) noexcept {
    return mode == EngagementMode::Open ? "open" : "invited";
}

const char *engagement_refusal_name(EngagementRefusal refusal) noexcept {
    switch (refusal) {
    case EngagementRefusal::None: return "ok";
    case EngagementRefusal::OptedOut: return "opted_out";
    case EngagementRefusal::NotInvited: return "not_invited";
    }
    return "unknown";
}

DoNotEngage load_do_not_engage(const std::filesystem::path &path) {
    DoNotEngage list;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return list;
    }
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read do-not-engage list " + path.string());
    }
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = line.find_last_not_of(" \t\r");
        const std::string did = line.substr(first, last - first + 1);
        if (protocol::is_did(did)) {
            list.dids.insert(did);
        } else {
            ++list.invalid_lines;
        }
    }
    return list;
}

std::set<std::string> invited_authors(const JournalContents &journal) {
    std::set<std::string> authors;
    for (const JournalEvent &event : journal.events) {
        if (!event.author_did.empty()) {
            authors.insert(event.author_did);
        }
    }
    return authors;
}

std::string action_target_did(const OutboundAction &action) {
    switch (action.kind) {
    case OutboundActionKind::Reply:
        return at_uri_authority(action.reply_parent);
    case OutboundActionKind::Like:
    case OutboundActionKind::Repost:
        return at_uri_authority(action.subject);
    case OutboundActionKind::Follow:
        return action.subject;
    case OutboundActionKind::Post:
    case OutboundActionKind::Unfollow:
    case OutboundActionKind::Moderation:
        return {};
    }
    return {};
}

EngagementRefusal check_engagement(OutboundActionKind kind, std::string_view target_did,
                                   const EngagementConfig &config,
                                   const std::set<std::string> &invited,
                                   const DoNotEngage &opted_out) {
    if (target_did.empty()) {
        return EngagementRefusal::None; /* nothing is being directed at anyone */
    }
    if (opted_out.dids.contains(std::string(target_did))) {
        return EngagementRefusal::OptedOut;
    }
    const bool needs_invitation = kind == OutboundActionKind::Like ||
                                  kind == OutboundActionKind::Repost ||
                                  kind == OutboundActionKind::Follow;
    if (needs_invitation && config.mode == EngagementMode::Invited &&
        !invited.contains(std::string(target_did))) {
        return EngagementRefusal::NotInvited;
    }
    return EngagementRefusal::None;
}

} // namespace atperson
