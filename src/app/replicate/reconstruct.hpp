#ifndef ATPERSON_REPLICATE_RECONSTRUCT_HPP
#define ATPERSON_REPLICATE_RECONSTRUCT_HPP

// Network reconstruct path (#142): rebuild a fresh entity state from
// published AT Protocol records. The inverse of the publisher — reads
// observation/action/valence records from an injectable RecordSource
// (Wolfram-backed in the network build, fake in tests), replays them
// into a fresh ledger and journal, then hands the result to the same
// rebuild path a local `atperson rebuild` uses.
//
// Observation records carry provenance and digest only — never observed
// text. Reconstruct re-fetches the content from the source URI through
// the record source and verifies the digest before replaying it. This
// is what makes publishing third-party content unnecessary: the network
// holds the identity of the experience, the source holds the bytes.
//
// Fail-closed on unavailable or digest-mismatched sources: a record
// whose content cannot be verified is reported in the result and NOT
// replayed. A hallucinated or tampered record cannot enter the ledger.

#include "records.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atperson {

/* Abstract record source: what a network-backed build implements with
 * Wolfram's record listing/fetch, and what tests fake. Ordered listing
 * by collection; fetch returns the raw record JSON. */
class RecordSource {
  public:
    virtual ~RecordSource() = default;

    /* rkeys for one collection, in stable (record-key) order. */
    [[nodiscard]] virtual std::vector<std::string> list_records(std::string_view collection) = 0;
    /* Raw record JSON for one rkey, or nullopt when the record is gone. */
    [[nodiscard]] virtual std::optional<std::string> get_record(std::string_view collection,
                                                                 std::string_view rkey) = 0;
    /* Re-fetch the observed content bytes for one source URI — the
     * reconstruct-side counterpart of the publisher's digest. Returns
     * nullopt when the source is unavailable. */
    [[nodiscard]] virtual std::optional<std::string> fetch_content(std::string_view source_uri) = 0;
};

struct ReconstructReport {
    std::uint64_t observations_replayed{0u};
    std::uint64_t actions_replayed{0u};
    std::uint64_t valence_replayed{0u};
    std::uint64_t thoughts_replayed{0u};
    std::uint64_t intents_replayed{0u};
    std::uint64_t observations_skipped_withdrawn{0u};
    std::uint64_t observations_failed{0u};
    std::uint64_t records_corrupt{0u};
    std::vector<std::string> failures;
};

/* Rebuild ledger + journal files from records. `ledger_path` and
 * `journal_path` must not exist — reconstruct is a fresh-state path, it
 * never merges into existing state. Throws std::runtime_error on local
 * I/O failure (the caller should treat that as a failed rebuild, not a
 * partial one). */
ReconstructReport reconstruct_state(RecordSource &source, const std::filesystem::path &ledger_path,
                                    const std::filesystem::path &journal_path,
                                    const std::filesystem::path &thoughts_path);

} // namespace atperson

#endif
