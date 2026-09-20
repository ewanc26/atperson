#ifndef ATPERSON_PROTOCOL_HPP
#define ATPERSON_PROTOCOL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson::protocol {

enum class Verification { Verified, Unverified, Rejected };
enum class XrpcKind { Query, Procedure, Subscription, Unknown };
enum class EvidenceKind { Identity, Repository, Record, Lexicon, Sync, Authorization };

struct AtUri {
    std::string did;
    std::string collection;
    std::string rkey;
};

/* Parse only the authority/collection/rkey form; query and fragment suffixes
 * are rejected so callers cannot accidentally learn from an ambiguous URI. */
std::optional<AtUri> parse_at_uri(std::string_view value);

/* NSIDs identify XRPC/Lexicon operations. The suffix is deliberately supplied
 * by the schema/event source rather than guessed from the name. */
XrpcKind classify_xrpc(bool query, bool procedure, bool subscription = false);

struct ProtocolEvidence {
    EvidenceKind kind{EvidenceKind::Repository};
    std::string source;
    std::string event_type;
    std::string subject;
    std::string payload;
    std::uint64_t sequence{};
    std::uint64_t observed_at{};
    Verification verification{Verification::Unverified};
    double confidence{};
};

/* In-memory authoritative reducer used by the durable adapter and tests. It
 * deduplicates evidence before reduction, keeping protocol knowledge separate
 * from the social observation ledger. */
class EvidenceStore {
  public:
    bool append(ProtocolEvidence evidence);
    [[nodiscard]] const std::vector<ProtocolEvidence> &entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] bool contains(std::string_view source, std::string_view payload) const;

  private:
    std::vector<ProtocolEvidence> entries_;
};

struct CursorState {
    std::uint64_t last_sequence{};
    std::string repo;
    std::string repo_revision;
    bool resync_required{};
};

enum class CursorResult { Initialized, Advanced, Duplicate, Gap, Rewind };
CursorResult observe_stream(CursorState &state, std::uint64_t sequence,
                            std::string_view repo, std::string_view revision);

} // namespace atperson::protocol

#endif
