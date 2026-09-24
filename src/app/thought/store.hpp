#ifndef ATPERSON_THOUGHT_STORE_HPP
#define ATPERSON_THOUGHT_STORE_HPP

// Thought store (#142): the durable, replayable record of the entity's
// own internal notes — self-authored text that is neither an outbound
// action nor an observation of someone else. A thought is the entity
// writing for its future self: a reflection, a summary, a note-to-self
// about what it noticed.
//
// The observation ledger is the authority for third-party experience;
// the action journal is the authority for outbound attempts. This store
// is the authority for self-authored internal experience. Unlike
// observations, thoughts are the entity's OWN words, so they publish in
// full to `click.croft.atperson.thought` — no digest-and-refetch dance
// is needed, and reconstruct replays the text directly.
//
// Storage: append-only JSONL at `<data>/thoughts.jsonl`. One compact
// JSON object per line, fsync after every append. A crash mid-append
// leaves at most one torn final line; loading reports and truncates it.
// Cross-process mutations serialise under the caller's lock, so the
// store itself performs no locking.
//
// Thoughts are NOT trained on automatically. They are experience the
// entity chose to write down; learning from them is a separate decision
// (and currently operator-injected via the normal observation path if
// wanted). The store's contract is durability and replay, not learning.
//
// Failure modes: ThoughtError for malformed entries and unsupported
// versions; std::runtime_error for I/O failure. A failed append is
// reported loudly and never rewrites earlier entries.

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace atperson {

inline constexpr std::uint32_t kThoughtFormatVersion = 1u;

class ThoughtError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* One self-authored internal note. `id` is a TID-shaped record key,
 * frozen at creation so the same thought always maps to the same
 * published record. `text` is the entity's own words. `kind` names the
 * thought's role — "reflection" for now; the vocabulary is closed and
 * validated so reconstruct never guesses. `about_uri` optionally links
 * the thought to the record it concerns (an at-URI); `at` is the
 * injected RFC 3339 UTC timestamp. */
struct Thought {
    std::string id;
    std::string kind;
    std::string text;
    std::string about_uri;
    std::string at;
};

/* Deterministic JSON for one thought (no trailing newline). */
[[nodiscard]] std::string serialise_thought(const Thought &entry);
[[nodiscard]] Thought parse_thought(std::string_view json);

/* True when `kind` is a valid thought kind. */
[[nodiscard]] bool thought_kind_is_valid(std::string_view kind);

/* Append one thought and fsync it. Creates the file and parent directory
 * when missing. Throws std::runtime_error on I/O failure. */
void append_thought(const std::filesystem::path &path, const Thought &entry);

/* The whole store in append order. A torn final line is truncated and
 * reported through *repaired_torn_tail; a malformed complete line throws
 * ThoughtError. A missing file yields an empty store. */
struct ThoughtContents {
    std::vector<Thought> thoughts;
    bool repaired_torn_tail{false};
};

[[nodiscard]] ThoughtContents load_thoughts(const std::filesystem::path &path);

} // namespace atperson

#endif
