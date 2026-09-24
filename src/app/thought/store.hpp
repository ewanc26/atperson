#ifndef ATPERSON_THOUGHT_STORE_HPP
#define ATPERSON_THOUGHT_STORE_HPP

// Thought store (#151): the durable, replayable record of the entity's own
// internal notes — self-authored text that is neither an outbound action nor
// an observation of someone else. A thought is the entity writing for its
// future self: a reflection, a consolidation, a movement notice.
//
// The observation ledger is the authority for third-party experience; the
// action journal is the authority for outbound attempts. This store is the
// authority for self-authored internal experience. Unlike observations,
// thoughts are the entity's OWN words, so a later network-native publish
// (#142) can carry them in full — no digest-and-refetch dance is needed, and
// reconstruct rehydrates the text directly.
//
// Storage: a directory of one JSON record per file. A thought is a single
// JSON record (AT Protocol is "just JSON", and a record is one JSON object),
// so each thought is `<data>/thoughts/<id>.json` — one file, one record, no
// delimiters to interpolate. Writing is atomic (tmp + rename + directory
// fsync) and append-only in effect: a record key is frozen at creation and a
// duplicate write is refused, so a crashed write can never corrupt an earlier
// thought. Cross-process mutations serialise under the caller's lock, so the
// store itself performs no locking.
//
// Thoughts are NOT trained on automatically. They are experience the entity
// chose to write down; learning from them is a separate decision (and
// currently operator-injected via the normal observation path if wanted).
// The deterministic reflection pass (#151) writes derived summaries into this
// store; it never feeds anything back into the learning loop.
//
// Kind vocabulary (closed, validated on both append and load):
//   "reflection"    hand-recorded note (`atperson thought <text...>`);
//   "consolidation" scheduled reflection-pass summary (daily cadence);
//   "movement"      threshold trigger notice emitted by the reflection pass.
//
// Provenance fields: `span_start`/`span_end` are the RFC 3339 analysis
// window the pass summarised (empty for hand-recorded thoughts); `topic` is
// the dedup subject key the pass uses so a rerun over the same window never
// duplicates a trigger. All three are optional; earlier files without them
// parse identically.
//
// Failure modes: ThoughtError for malformed records and unsupported
// versions; std::runtime_error for I/O failure. A failed write is reported
// loudly and never disturbs any other record.

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

inline constexpr std::uint32_t kThoughtFormatVersion = 1u;

class ThoughtError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* One self-authored internal note. `id` is a TID-shaped record key, frozen
 * at creation so the same thought always maps to the same published record.
 * `text` is the entity's own words. `kind` names the thought's role (see the
 * closed vocabulary above). `about_uri` optionally links the thought to the
 * record it concerns (an at-URI); `at` is the injected RFC 3339 UTC
 * timestamp. `span_start`/`span_end`/`topic` are pass provenance (empty for
 * hand-recorded thoughts). */
struct Thought {
    std::string id;
    std::string kind;
    std::string text;
    std::string about_uri;
    std::string at;
    std::string span_start;
    std::string span_end;
    std::string topic;
};

/* True when `kind` is a valid thought kind. */
[[nodiscard]] bool thought_kind_is_valid(std::string_view kind);

/* A TID-shaped record key for the current microsecond clock: 13 base-32
 * characters, monotonic enough for a local store. Its lexicographic order is
 * creation order, which is also the store's read order. */
[[nodiscard]] std::string new_thought_id();

/* Deterministic JSON for one thought (one record, no trailing newline). */
[[nodiscard]] std::string serialise_thought(const Thought &entry);
[[nodiscard]] Thought parse_thought(std::string_view json);

/* The file that holds one record: `<dir>/<id>.json`. */
[[nodiscard]] std::filesystem::path thought_record_path(const std::filesystem::path &dir,
                                                        std::string_view id);

/* True when `<dir>/<id>.json` already exists. */
[[nodiscard]] bool thought_record_exists(const std::filesystem::path &dir, std::string_view id);

/* Atomically write one record to `<dir>/<id>.json` (tmp + rename + directory
 * fsync). Creates the directory when missing. Refuses a duplicate id — a
 * recorded thought is immutable. Throws std::runtime_error on I/O failure. */
void write_thought(const std::filesystem::path &dir, const Thought &entry);

/* The whole store, sorted by record key (creation order). A malformed record
 * file throws ThoughtError; a missing directory yields an empty store. Files
 * that are not `<id>.json` records (for example a leftover `<id>.json.tmp`)
 * are ignored. */
struct ThoughtContents {
    std::vector<Thought> thoughts;
};

[[nodiscard]] ThoughtContents load_thoughts(const std::filesystem::path &dir);

} // namespace atperson

#endif