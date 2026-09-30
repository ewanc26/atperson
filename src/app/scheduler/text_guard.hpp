#ifndef ATPERSON_SCHEDULER_TEXT_GUARD_HPP
#define ATPERSON_SCHEDULER_TEXT_GUARD_HPP

// Output guard for text the entity publishes on its own.
//
// The autonomous scheduler composes post/reply text from learned tokens, and
// the learned tokens come from whatever public text the entity has read. Nothing
// about the guarded decision layer says the result is safe to publish
// unattended, so this is the last check before an autonomous post is proposed
// and again before it is executed. It is what a conventional agent calls an
// output guardrail, kept deterministic and inspectable: no model, no scoring,
// just named rules and a stable reason code for every refusal.
//
// What it refuses, in order:
//   empty           nothing to say
//   invalid_utf8    Bluesky records must be valid UTF-8
//   too_long        more than `max_codepoints` (Bluesky's post limit is 300)
//   url             a scheme (`://`), `www.`, a domain-shaped word, or the
//                   words http/https/www
//   mention         an `@` followed by a word character, or a DID (`did:`)
//   hashtag         a `#` followed by a word character
//   denied_term     a term from the operator's denylist
//   repeated_text   the same text (case, spacing and punctuation ignored) was
//                   already published or is already waiting, inside the window
//
// URLs, mentions and hashtags are refused unconditionally for autonomous text:
// they are how an unattended account becomes a spam source, and nothing the
// entity learned justifies them. Operator-authored `publish` is not routed
// through this guard; an operator can post whatever they choose.
//
// The denylist is operator policy, not persona: the entity ships with no
// opinions and an empty denylist, and the operator decides what it must never
// say. It is a file of one term per line (`#` starts a comment), read every
// cycle so an edit takes effect on the next one without a restart. A term
// matches as a whole word, case-insensitively (ASCII case folding); a term
// containing a space or punctuation matches as a phrase.
//
// Pure apart from `load_denylist`; the clock is always injected.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

struct TextGuardConfig {
    std::size_t max_codepoints{300};
    /* Lower-cased terms; see the header comment for matching. */
    std::vector<std::string> denied_terms;
    /* How long an identical text counts as a repeat. 0 disables the check. */
    std::int64_t repeat_window_seconds{7ll * 24ll * 60ll * 60ll};
};

enum class TextRefusal {
    None,
    Empty,
    InvalidUtf8,
    TooLong,
    Url,
    Mention,
    Hashtag,
    DeniedTerm,
    RepeatedText,
};

/* Stable machine name ("url", "denied_term", ...); "ok" for None. */
[[nodiscard]] const char *text_refusal_name(TextRefusal refusal) noexcept;

struct TextVerdict {
    TextRefusal refusal{TextRefusal::None};
    /* What tripped it: the matched term, the length, the earlier time. */
    std::string detail;
    [[nodiscard]] bool allowed() const noexcept { return refusal == TextRefusal::None; }
};

/* A text the entity already published (or has queued), for repeat detection. */
struct RecentText {
    std::string text;
    std::int64_t at_epoch{0};
};

/* Case, spacing and punctuation-insensitive form used for repeat detection. */
[[nodiscard]] std::string normalize_for_repeat(std::string_view text);

[[nodiscard]] TextVerdict check_output_text(std::string_view text, const TextGuardConfig &config,
                                            const std::vector<RecentText> &recent,
                                            std::int64_t now_epoch);

/* One term per line; blank lines and `#` comments ignored; terms are lower-cased
 * and trimmed. A missing file is an empty denylist. Throws std::runtime_error
 * only for an I/O failure on a file that exists. */
[[nodiscard]] std::vector<std::string> load_denylist(const std::filesystem::path &path);

} // namespace atperson

#endif
