#ifndef ATPERSON_JOURNAL_STORE_HPP
#define ATPERSON_JOURNAL_STORE_HPP

// Action/outcome journal (#27): the durable, replayable record of the entity's
// own outbound actions and what happened to them afterwards.
//
// The observation ledger is the authority for *third-party* experience; this
// journal is the authority for *self-authored* experience. It links, for every
// outbound attempt, the exact action (rkey, text, approval digest), the
// runtime's execution outcome (executed / denied / deferred / failed /
// dry-run), the network result (at-URI, CID) and — later — the public
// replies/quotes that reference the executed record. Executed-action
// provenance stays separate from ordinary observations while remaining
// auditable and reconstructable.
//
// The journal deliberately overlaps the #25 outbound audit log (both record
// every attempt). They serve different contracts: the audit log is the
// operator-facing operational record of execution; the journal is experience
// provenance that events and valence entries hang off, and it is the replay
// source for explicit state updates. Neither is derived from the other.
//
// Storage: append-only JSONL at `<data>/action-journal.jsonl`. One compact
// JSON object per line, fsync after every append. A crash mid-append leaves
// at most one torn final line; loading reports and truncates it rather than
// silently reinterpreting partial bytes. Cross-process mutations serialise
// under the caller's lock (the daemon holds the writer lock; `publish` and
// the valence command hold the outbound lock), so the store itself performs
// no locking.
//
// Nothing here feeds learning. Valence is applied only through the explicit
// C23 API (atp_graph_valence_event) by the operator-facing command in
// journal/command.cpp; the journal records that it happened so a rebuild
// can replay it deterministically.
//
// Ownership: entries are payload-by-value; nothing allocates beyond
// std::string/vector. No network, no Wolfram, no clock — timestamps are
// injected by callers.
//
// Failure modes: JournalError for malformed entries, unsupported versions and
// impossible field combinations; std::runtime_error for I/O failure. A failed
// append is reported loudly and never rewrites earlier entries.

#include "atperson/core.h"
#include "journal/mac.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

inline constexpr std::uint32_t kJournalFormatVersion = 4u;

/* What happened to one attempted outbound action. Mirrors the #25 execution
 * outcome vocabulary exactly, so the journal never invents a third spelling
 * for the same fact. */
enum class JournalActionOutcome { Executed, Denied, Deferred, Failed, DryRun };

[[nodiscard]] const char *journal_action_outcome_name(JournalActionOutcome outcome) noexcept;
[[nodiscard]] std::optional<JournalActionOutcome>
journal_action_outcome_from_name(std::string_view name);

/* The derived lifetime state of one action's recorded expectation (#149).
 * `None` — the action carries no expectation (operator-authored) or is not
 * an executed action; `Pending` — no linked event yet and the expectation
 * window has not closed; `Met` — at least one linked event landed before the
 * window closed; `Unmet` — a linked event exists but none landed in time
 * (a late reference); `Expired` — the window closed with no linked event.
 * Only the three terminal states (`Met`, `Unmet`, `Expired`) are ever
 * written as resolution lines; `None`/`Pending` are derived, not stored. */
enum class JournalExpectationState { None, Pending, Met, Unmet, Expired };

[[nodiscard]] const char *
journal_expectation_state_name(JournalExpectationState state) noexcept;
[[nodiscard]] std::optional<JournalExpectationState>
journal_expectation_state_from_name(std::string_view name);

/* Map a valence kind name ("action", "interaction", "approach", "avoid") to
 * the C23 atp_valence_kind enum. Symmetric to journal_action_outcome_from_name
 * and used by the journal apply command. Unknown names return nullopt. */
[[nodiscard]] std::optional<atp_valence_kind> valence_kind_from_name(std::string_view name);

/* The canonical name for a valence kind; the same four spellings
 * valence_kind_from_name accepts. Used by `journal map` (#56) so derived
 * entries use the journal's kind vocabulary, never a third spelling. */
[[nodiscard]] const char *valence_kind_name(atp_valence_kind kind) noexcept;

/* The predicted outcome of an executed action (#149): which valence kind the
 * decision most expected, how likely the entity judged a reply, and which
 * tokens it expected to be reinforced. `kind` is one of the closed
 * vocabulary {"action", "interaction", "approach"}; `reply_likelihood` is
 * clamped to [0, 1]; `tokens` are the text's distinct tokens in
 * first-appearance order, capped at kExpectationMaxTokens. Derived from the
 * frozen outbound document, deterministically, never from the graph's
 * current state — reconstruct replay derives the same expectation. */
struct JournalExpectation {
    std::string kind;
    float reply_likelihood{};
    std::vector<std::string> tokens;
};

/* Expected-token cap for an action expectation (#149): a serialised
 * expectation stays small regardless of plan length. */
inline constexpr std::size_t kExpectationMaxTokens = 8u;

/* One attempted outbound action. `id` is the frozen rkey from the #25 action
 * document: stable up front, idempotent under putRecord retry, and the tail
 * of the executed record's at-URI, so events link to actions by identifier
 * rather than by matching mutable text. `digest` is the #22 approval digest
 * (16 lowercase hex). `uri`/`cid` are populated only when Executed.
 * `mac` holds the journal-integrity MAC when a MAC key is configured
 * (#57, scoped). `expectation` is the predicted outcome recorded at
 * execution time (#149): present only for autonomous decisions, and the
 * subject of the resolution pass. */
struct JournalAction {
    std::string id;
    std::string kind; /* "post" or "reply" */
    std::string text;
    std::string digest;
    JournalActionOutcome outcome{JournalActionOutcome::Denied};
    std::string reason; /* stable machine reason code */
    std::string uri;
    std::string cid;
    std::string at; /* RFC 3339 UTC timestamp of the attempt */
    std::optional<JournalMac> mac; /* journal-integrity MAC (#57, scoped) */
    std::optional<JournalExpectation> expectation; /* predicted outcome (#149) */
};

/* One recorded outcome of an action expectation (#149): a terminal
 * resolution-pass state (`Met`, `Unmet` or `Expired`) for one executed
 * action. Only terminal states are written; a pending expectation has no
 * line. `at`/`at_epoch` are when the pass recorded the state. */
struct JournalResolution {
    std::string action_id;
    JournalExpectationState state{JournalExpectationState::Met};
    std::uint64_t at_epoch{};
    std::string at; /* RFC 3339 UTC timestamp of the pass */
};

/* The lifetime state of one pending social intent (#150). `Open` — the
 * conversation is being waited on (replies may still arrive); `Expired` —
 * the reply window closed without a continuation trigger; `Closed` — the
 * intent reached its continuation budget (max_continuations replies), so
 * the entity stops orbiting the thread. Only the two terminal states are
 * written by the expiry sweep; `Open` is the recorded state of every live
 * and continuation entry. */
enum class IntentState { Open, Expired, Closed };

[[nodiscard]] const char *intent_state_name(IntentState state) noexcept;
[[nodiscard]] std::optional<IntentState> intent_state_from_name(std::string_view name);

/* One durable intent entry (#150): a record of one conversation the entity
 * is holding — an executed post/reply that invites a response, the thread
 * it belongs to, who the reply is expected from, and a bounded reply
 * window. The journal is append-only, so an intent's *current* record is
 * the last entry with its id: opening appends one line, every continuation
 * appends another with the conversation's actions grown by one, and the
 * expiry sweep appends the terminal state. `id` is the thread-root at-URI
 * (stable conversation identity, so a reply received in the same thread
 * finds its intent). `actions` lists the entity's own action ids in this
 * conversation, oldest first; the back is the latest anchor that later
 * journal events link through. `responder` is "anyone" or "did:<author>".
 * `continuations` used equals actions.size() - 1, so the max_continuations
 * cap is derivable from the entry — no second counter store. */
struct JournalIntent {
    std::string id;
    std::vector<std::string> actions;
    std::string responder; /* "anyone" or "did:<author>" */
    std::uint64_t expires_at_epoch{};
    std::string expires_at; /* RFC 3339 UTC window close */
    std::uint32_t max_continuations{3u};
    IntentState state{IntentState::Open};
    std::uint64_t at_epoch{};
    std::string at; /* RFC 3339 UTC when this entry was recorded */
};

/* How a later public record referenced an executed action. `via` names the
 * referencing field: "parent" (a direct reply), "root" (a reply in the same
 * thread) or "quote" (a quote post). */
struct JournalEvent {
    std::string action_id;
    std::string event_uri;
    std::string author_did;
    std::string via;
    std::string at; /* RFC 3339 UTC timestamp of the event record */
};

/* One explicit experience-derived state update the operator applied (#13).
 * The journal stores what was applied so `rebuild` can replay it after the
 * ledger; it never applies anything itself. `kind` is a valence kind name
 * ("action", "interaction", "approach", "avoid"), `signal` the clamped
 * [-1, 1] value, `source` the journal action id or AT URI the event cites.
 * `provenance` records how the entry was produced: empty for a hand-run
 * `journal apply`, "map:<rule-id>" for an entry derived by `journal map`
 * (#56). It is inspection metadata only — replay applies every valence
 * entry identically regardless of provenance. */
struct JournalValence {
    std::string token;
    std::string kind;
    float signal{};
    std::string source;
    std::uint64_t at_epoch{};
    std::string at; /* RFC 3339 UTC timestamp of the application */
    std::string provenance; /* empty (apply) or "map:<rule-id>" (#56) */
};

/* Malformed or unsupported journal content. Corruption is reported, never
 * silently reinterpreted. */
class JournalError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Deterministic JSON for each entry kind (no trailing newline). */
[[nodiscard]] std::string serialise_journal_action(const JournalAction &entry);
[[nodiscard]] std::string serialise_journal_event(const JournalEvent &entry);
[[nodiscard]] std::string serialise_journal_valence(const JournalValence &entry);
[[nodiscard]] std::string serialise_journal_resolution(const JournalResolution &entry);
[[nodiscard]] std::string serialise_journal_intent(const JournalIntent &entry);

/* Append one entry and fsync it. Creates the file and parent directory when
 * missing. Throws std::runtime_error on I/O failure. */
void append_journal_action(const std::filesystem::path &path, const JournalAction &entry);
void append_journal_event(const std::filesystem::path &path, const JournalEvent &entry);
void append_journal_valence(const std::filesystem::path &path, const JournalValence &entry);
void append_journal_resolution(const std::filesystem::path &path,
                               const JournalResolution &entry);
void append_journal_intent(const std::filesystem::path &path, const JournalIntent &entry);

/* The whole journal in append order. A torn final line is truncated and
 * reported through *repaired_torn_tail; a malformed complete line throws
 * JournalError. A missing file yields an empty journal. */
struct JournalContents {
    std::vector<JournalAction> actions;
    std::vector<JournalEvent> events;
    std::vector<JournalValence> valence;
    std::vector<JournalResolution> resolutions;
    std::vector<JournalIntent> intents;
    bool repaired_torn_tail{false};
};

[[nodiscard]] JournalContents load_journal(const std::filesystem::path &path);

/* True when (action id, event uri) is already recorded. Restart and
 * re-ingestion dedup for event linkage. */
[[nodiscard]] bool journal_has_event(const JournalContents &journal, std::string_view action_id,
                                     std::string_view event_uri);

/* The executed action whose result at-URI is `uri`, or nullptr. The linkage
 * key: an incoming observation's reply parent/root/quote URI is looked up
 * here, never matched against action text. */
[[nodiscard]] const JournalAction *journal_find_action_by_uri(const JournalContents &journal,
                                                              std::string_view uri);

} // namespace atperson

#endif
