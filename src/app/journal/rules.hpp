#ifndef ATPERSON_JOURNAL_RULES_HPP
#define ATPERSON_JOURNAL_RULES_HPP

// Outcome-to-valence rule table (#56): the operator-authored mapping from
// journal outcomes onto explicit valence events.
//
// The action/outcome journal (#27) records what the entity did and what
// happened afterwards, but nothing maps those outcomes onto valence: the
// operator had to run `journal apply` by hand for every event. This module
// is that mapping as explicit, inspectable policy — a bounded rule table
// the operator authors, evaluated deterministically. It is not a learned
// function, and it is never a side effect of `publish`, `sync` or the
// daemon: the operator runs `journal map` deliberately.
//
// A rule names a valence kind ("action" / "interaction" / "approach" /
// "avoid"), a signal in [-1, 1], and a trigger drawn from journal fields:
// the action's execution outcome, optionally a minimum count of later
// events (replies/quotes) and optionally a window in seconds after the
// action's attempt within which those events must fall. Example: an
// executed post that received a reply within 24 hours scores
// `interaction +0.5`; a denied post scores `action -0.5`.
//
// Evaluation is fixed and deterministic: rules run in table order, first
// match wins, and no rule fires for an outcome the operator has not
// explicitly mapped. An empty or absent rule table derives nothing.
//
// Format: JSON, one object:
//   {"format": "atperson-valence-rules", "version": 1, "rules": [
//     {"id": "denied-negative", "when": {"outcome": "denied"},
//      "kind": "action", "signal": -0.5},
//     {"id": "replied-positive", "when": {"outcome": "executed",
//      "min_events": 1, "within_seconds": 86400},
//      "kind": "interaction", "signal": 0.5}
//   ]}
// The rule table is text the operator can read and diff. It is not
// persisted by atperson — the operator owns the file — and no learned
// state depends on it: `rebuild` replays the valence entries the mapping
// produced, not the mapping itself.
//
// Failure modes: JournalError for malformed tables, unsupported versions,
// duplicate rule ids, unknown outcome/kind names and out-of-range values.
// std::runtime_error for I/O failure. A rule that does not match is not a
// failure.

#include "store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atperson {
namespace journal {

inline constexpr std::string_view kRuleFormat = "atperson-valence-rules";
inline constexpr std::uint32_t kRuleFormatVersion = 1u;
inline constexpr std::size_t kMaxRules = 256u;
inline constexpr std::uint64_t kMaxWithinSeconds = 10u * 365u * 86400u;

/* One operator-authored mapping rule. `when` is the trigger: the action's
 * outcome, plus optional bounds on the later events linked to it. */
struct ValenceRule {
    std::string id;
    JournalActionOutcome outcome{JournalActionOutcome::Denied};
    std::optional<std::uint32_t> min_events; /* >= 1 when present */
    std::optional<std::uint64_t> within_seconds; /* window after the attempt */
    atp_valence_kind kind{ATP_VALENCE_ACTION};
    float signal{}; /* clamped [-1, 1] at parse time */
};

/* The whole rule table in table order. */
struct RuleTable {
    std::vector<ValenceRule> rules;
};

/* Parse and validate a rule table from a JSON string. Throws JournalError
 * on malformed content; std::runtime_error never (parsing is pure). */
[[nodiscard]] RuleTable parse_rule_table(std::string_view json_text);

/* Read and parse a rule table from a file. Throws std::runtime_error when
 * the file cannot be read; JournalError when the content is malformed. */
[[nodiscard]] RuleTable load_rule_table(const std::filesystem::path &path);

/* The first rule in table order whose trigger matches `action` (given the
 * events linked to it, already filtered to the action's id), or nullptr
 * when no rule maps this outcome. First match wins; unmapped outcomes
 * produce nothing. */
[[nodiscard]] const ValenceRule *first_matching_rule(const RuleTable &table,
                                                      const JournalAction &action,
                                                      const std::vector<JournalEvent> &events);

} // namespace journal
} // namespace atperson

#endif
