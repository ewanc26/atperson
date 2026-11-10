#ifndef ATPERSON_AUTONOMY_BOT_LABEL_HPP
#define ATPERSON_AUTONOMY_BOT_LABEL_HPP

// The "bot" self-label on an account's profile record.
//
// Bluesky's guidance for automated accounts is to identify themselves with the
// global `bot` self-label, and its guidelines prohibit deceptive account
// practices. An entity that posts on its own and does not say so is exactly the
// case that guidance is about. The label lives in the account's
// `app.bsky.actor.profile` record (`labels: {$type: com.atproto.label.defs#selfLabels,
// values: [{val: "bot"}]}`), so setting it means rewriting the whole profile
// record. That is the dangerous part: a careless rewrite drops the avatar, the
// display name or the description.
//
// This module is the safe part, and is pure. `set_profile_self_label` edits the
// record JSON and preserves every other field, and its order; it adds or removes
// only the one label value, keeps any other labels, and REFUSES (BotLabelError)
// a profile whose `labels` field has a shape it does not understand rather than
// overwrite it. Reading and writing the record belongs to the Wolfram adapter and
// the operator-run command.
//
// Failure modes: BotLabelError for JSON that is not a profile object or a labels
// field of an unrecognised shape.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

class BotLabelError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::string_view kProfileCollection = "app.bsky.actor.profile";
inline constexpr std::string_view kProfileRecordKey = "self";
inline constexpr std::string_view kBotLabel = "bot";

/* The record used when the account has no profile record at all. */
[[nodiscard]] std::string empty_profile_json();

/* True when the profile carries `label` as a self-label. */
[[nodiscard]] bool profile_has_self_label(std::string_view profile_json, std::string_view label);

struct LabelEdit {
    std::string json;  /* the resulting record JSON */
    bool changed{false};
};

/* Ensure `label` is (present=true) or is not (present=false) among the profile's
 * self-labels, changing nothing else. */
[[nodiscard]] LabelEdit set_profile_self_label(std::string_view profile_json,
                                               std::string_view label, bool present);

/* A local note of what the operator last confirmed, so the offline preflight can
 * say whether the entity has identified itself. It is a convenience, never an
 * authority: the profile record is, and `autonomy label status` re-checks it. */
struct LabelMarker {
    bool present{false};
    std::int64_t checked_at{0};
};

[[nodiscard]] std::optional<LabelMarker> read_label_marker(const std::filesystem::path &path);
void write_label_marker(const std::filesystem::path &path, const LabelMarker &marker);

} // namespace atperson

#endif
