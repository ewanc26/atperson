#ifndef ATPERSON_REPLICATE_RECORDS_HPP
#define ATPERSON_REPLICATE_RECORDS_HPP

// Network-native state record shapes (#142): the JSON documents the
// entity's durable experience is serialised to and parsed from on the
// AT Protocol. Pure data mapping — no I/O, no Wolfram, fully offline.
//
// Three record kinds, all under the entity's own DID:
//
//   observation — provenance for one ledger entry: source identity,
//                 author, timestamps, content digest, outcome and
//                 conversational context. NEVER the observed text:
//                 third-party content is not republished. Reconstruction
//                 re-fetches from the source URI and verifies the digest.
//   action      — one self-authored outbound action from the journal.
//   valence     — one self-authored explicit valence application.
//   thought     — one self-authored internal note from the thought
//                 store. Unlike observations, thoughts are the entity's
//                 OWN words, so the record carries the full text: no
//                 digest-and-refetch, no third-party content concern.
//
// The shapes encode exactly what the local ledger/journal store — the
// same canonical payloads, the same field semantics — so a network
// rebuild replays through the same C23 path as a local rebuild.
//
// Failure mode: RecordError on malformed input. Corruption is reported,
// never silently reinterpreted.

#include "atperson/conversation.hpp"
#include "atperson/core.h"
#include "journal/store.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {

class RecordError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Collection NSIDs. All ATperson-owned lexicons live under the
// click.croft.atperson authority; the publisher and reconstruct paths
// agree on these constants. */
inline constexpr std::string_view kObservationCollection =
    "click.croft.atperson.observation";
inline constexpr std::string_view kActionCollection = "click.croft.atperson.action";
inline constexpr std::string_view kValenceCollection = "click.croft.atperson.valence";
inline constexpr std::string_view kThoughtCollection = "click.croft.atperson.thought";

/* Record format marker and version. A format change that alters what a
 * rebuild learns requires a version bump and reconstruct-side migration
 * decision, exactly like ATPERSON_SCHEMA_VERSION. */
inline constexpr std::string_view kRecordFormat = "atperson-record";
inline constexpr std::uint32_t kRecordFormatVersion = 1u;

/* One observation provenance record. Field semantics are the ledger
 * entry's exactly: `id` is the ledger entry id (stable, monotonic),
 * `contentDigest` is the 64-bit FNV digest of the canonical text as
 * Ledger::digest computes it, serialised as a decimal string — a u64
 * does not survive a JSON number round-trip (doubles carry 53 bits).
 * `outcome` is the FINAL outcome at publish
 * time — a withdrawal republishes the same rkey with outcome
 * "withdrawn", so a network rebuild excludes the experience. */
struct ObservationRecord {
    std::uint64_t id{};
    std::string source_id;
    std::string author_did;
    std::uint64_t observed_at{};
    std::uint64_t content_digest{};
    std::uint32_t schema_version{};
    std::string outcome; /* "pending"|"learned"|"skipped"|"failed"|"withdrawn" */
    ConversationContext context;
};

[[nodiscard]] std::string serialise_observation_record(const ObservationRecord &record);
[[nodiscard]] ObservationRecord parse_observation_record(std::string_view json);

/* One self-authored action record. Mirrors JournalAction exactly; `id`
 * is the frozen action rkey. */
struct ActionRecord {
    std::string id;
    std::string kind;
    std::string text;
    std::string digest;
    std::string outcome; /* journal action outcome name */
    std::string reason;
    std::string uri;
    std::string cid;
    std::string at;
};

[[nodiscard]] std::string serialise_action_record(const ActionRecord &record);
[[nodiscard]] ActionRecord parse_action_record(std::string_view json);

/* One self-authored valence record. Mirrors JournalValence exactly. */
struct ValenceRecord {
    std::string token;
    std::string kind;
    float signal{};
    std::string source;
    std::uint64_t at_epoch{};
    std::string at;
    std::string provenance;
};

[[nodiscard]] std::string serialise_valence_record(const ValenceRecord &record);
[[nodiscard]] ValenceRecord parse_valence_record(std::string_view json);

/* One self-authored thought record. Mirrors the Thought store entry
 * exactly; `id` is the frozen TID-shaped record key. */
struct ThoughtRecord {
    std::string id;
    std::string kind;
    std::string text;
    std::string about_uri;
    std::string at;
};

[[nodiscard]] std::string serialise_thought_record(const ThoughtRecord &record);
[[nodiscard]] ThoughtRecord parse_thought_record(std::string_view json);

/* Stable outcome names shared by the record layer and the ledger. */
[[nodiscard]] std::string_view ledger_outcome_name(atp_ledger_outcome outcome);
[[nodiscard]] std::optional<atp_ledger_outcome> ledger_outcome_from_name(
    std::string_view name);

} // namespace atperson

#endif
