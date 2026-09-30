#ifndef ATPERSON_SCHEDULER_ENGAGEMENT_HPP
#define ATPERSON_SCHEDULER_ENGAGEMENT_HPP

// Engagement consent: whom an unattended account may interact with.
//
// Bluesky's own guidance for bots is explicit: only interact (like, repost,
// reply) with a user who has engaged with the bot first, otherwise the account
// risks being flagged as spam, and indiscriminate volume is what the platform's
// anti-abuse rules target. The scheduler's original behaviour did not respect
// that: a graduated like could target any post it happened to have read.
//
// Two rules, both deterministic and inspectable:
//
//   opt-out    an operator-maintained do-not-engage list of DIDs. The entity
//              never likes, reposts, follows or replies to anyone on it, in
//              any mode. It is a file, one DID per line (`#` comments), read
//              every cycle so adding someone takes effect on the next cycle
//              with no restart. Invalid lines are ignored at runtime (a typo
//              must not stop the entity) and counted, and the preflight
//              reports them as a setup fault so they are caught.
//   consent    in `invited` mode (the default) a like, repost or follow needs
//              the target's author to have engaged with the entity first:
//              their post replied to or quoted something the entity published
//              (a journal event). `open` mode drops that requirement (the
//              opt-out list still applies) and is an explicit operator choice.
//
// Replies are always continuations of a conversation the entity started, which
// is itself an invitation, so they are only subject to the opt-out list. Plain
// posts have no target.
//
// Pure apart from `load_do_not_engage`.

#include "../journal/store.hpp"
#include "../outbound/action.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

namespace atperson {

enum class EngagementMode { Invited, Open };

struct EngagementConfig {
    EngagementMode mode{EngagementMode::Invited};
};

[[nodiscard]] const char *engagement_mode_name(EngagementMode mode) noexcept;

enum class EngagementRefusal { None, OptedOut, NotInvited };

/* Stable machine name ("opted_out", "not_invited"); "ok" for None. */
[[nodiscard]] const char *engagement_refusal_name(EngagementRefusal refusal) noexcept;

struct DoNotEngage {
    std::set<std::string> dids;
    /* Non-blank, non-comment lines that are not a DID. */
    std::size_t invalid_lines{0};
};

/* Missing file = nobody opted out. Throws std::runtime_error only for an I/O
 * failure on a file that exists. */
[[nodiscard]] DoNotEngage load_do_not_engage(const std::filesystem::path &path);

/* Authors who have engaged with the entity: those with a journal event
 * (a reply or quote of something the entity published). */
[[nodiscard]] std::set<std::string> invited_authors(const JournalContents &journal);

/* The DID an action is directed at, or empty when it has no target: a reply's
 * parent author, a like/repost subject's author, a follow's subject. */
[[nodiscard]] std::string action_target_did(const OutboundAction &action);

[[nodiscard]] EngagementRefusal check_engagement(OutboundActionKind kind,
                                                 std::string_view target_did,
                                                 const EngagementConfig &config,
                                                 const std::set<std::string> &invited,
                                                 const DoNotEngage &opted_out);

} // namespace atperson

#endif
