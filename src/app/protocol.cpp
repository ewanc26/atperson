#include "atperson/protocol.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace {

void write_field(std::ostream &out, std::string_view value) {
    const std::uint64_t size = value.size();
    out.write(reinterpret_cast<const char *>(&size), sizeof(size));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_field(std::istream &in) {
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char *>(&size), sizeof(size));
    if (!in || size > 16u * 1024u * 1024u) throw std::runtime_error("invalid protocol evidence field");
    std::string value(size, '\0');
    in.read(value.data(), static_cast<std::streamsize>(size));
    if (!in) throw std::runtime_error("truncated protocol evidence ledger");
    return value;
}

} // namespace

namespace atperson::protocol {

std::optional<AtUri> parse_at_uri(std::string_view value) {
    if (!value.starts_with("at://") || value.find_first_of("?#") != std::string_view::npos) {
        return std::nullopt;
    }
    value.remove_prefix(5);
    const auto first = value.find('/');
    if (first == std::string_view::npos || first == 0) return std::nullopt;
    const auto second = value.find('/', first + 1);
    if (second == std::string_view::npos || second == first + 1 || second + 1 == value.size()) {
        return std::nullopt;
    }
    AtUri out{std::string(value.substr(0, first)),
              std::string(value.substr(first + 1, second - first - 1)),
              std::string(value.substr(second + 1))};
    if (!out.did.starts_with("did:") || out.collection.find('.') == std::string::npos ||
        out.rkey.find('/') != std::string::npos) {
        return std::nullopt;
    }
    return out;
}

XrpcKind classify_xrpc(bool query, bool procedure, bool subscription) {
    const int count = static_cast<int>(query) + static_cast<int>(procedure) +
                      static_cast<int>(subscription);
    if (count != 1) return XrpcKind::Unknown;
    if (query) return XrpcKind::Query;
    if (procedure) return XrpcKind::Procedure;
    return XrpcKind::Subscription;
}

bool EvidenceStore::contains(std::string_view source, std::string_view payload) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const auto &entry) {
        return entry.source == source && entry.payload == payload;
    });
}

bool EvidenceStore::append(ProtocolEvidence evidence) {
    if (contains(evidence.source, evidence.payload)) return false;
    entries_.push_back(std::move(evidence));
    return true;
}

EvidenceLedger::EvidenceLedger(const std::filesystem::path &path) : path_(path) {
    if (std::filesystem::exists(path_) && std::filesystem::file_size(path_) == 0) {
        throw std::runtime_error("empty protocol evidence ledger");
    }
}

EvidenceLedger::~EvidenceLedger() = default;

bool EvidenceLedger::append(ProtocolEvidence evidence) {
    EvidenceStore current;
    for (auto &entry : entries()) current.append(std::move(entry));
    if (!current.append(evidence)) return false;
    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("open protocol evidence ledger");
    const std::uint32_t magic = 0x41545045u;
    out.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    const auto kind = static_cast<std::uint8_t>(evidence.kind);
    const auto verification = static_cast<std::uint8_t>(evidence.verification);
    out.write(reinterpret_cast<const char *>(&kind), sizeof(kind));
    out.write(reinterpret_cast<const char *>(&verification), sizeof(verification));
    out.write(reinterpret_cast<const char *>(&evidence.sequence), sizeof(evidence.sequence));
    out.write(reinterpret_cast<const char *>(&evidence.observed_at), sizeof(evidence.observed_at));
    out.write(reinterpret_cast<const char *>(&evidence.confidence), sizeof(evidence.confidence));
    write_field(out, evidence.source);
    write_field(out, evidence.event_type);
    write_field(out, evidence.subject);
    write_field(out, evidence.payload);
    if (!out) throw std::runtime_error("write protocol evidence ledger");
    return true;
}

std::vector<ProtocolEvidence> EvidenceLedger::entries() const {
    std::vector<ProtocolEvidence> result;
    if (!std::filesystem::exists(path_)) return result;
    std::ifstream in(path_, std::ios::binary);
    while (in.peek() != std::char_traits<char>::eof()) {
        std::uint32_t magic = 0;
        in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
        if (!in || magic != 0x41545045u) throw std::runtime_error("invalid protocol evidence ledger");
        std::uint8_t kind = 0, verification = 0;
        ProtocolEvidence evidence;
        in.read(reinterpret_cast<char *>(&kind), sizeof(kind));
        in.read(reinterpret_cast<char *>(&verification), sizeof(verification));
        in.read(reinterpret_cast<char *>(&evidence.sequence), sizeof(evidence.sequence));
        in.read(reinterpret_cast<char *>(&evidence.observed_at), sizeof(evidence.observed_at));
        in.read(reinterpret_cast<char *>(&evidence.confidence), sizeof(evidence.confidence));
        if (!in || kind > static_cast<std::uint8_t>(EvidenceKind::Authorization) ||
            verification > static_cast<std::uint8_t>(Verification::Rejected)) {
            throw std::runtime_error("invalid protocol evidence header");
        }
        evidence.kind = static_cast<EvidenceKind>(kind);
        evidence.verification = static_cast<Verification>(verification);
        evidence.source = read_field(in);
        evidence.event_type = read_field(in);
        evidence.subject = read_field(in);
        evidence.payload = read_field(in);
        result.push_back(std::move(evidence));
    }
    return result;
}

CursorResult observe_stream(CursorState &state, std::uint64_t sequence,
                            std::string_view repo, std::string_view revision) {
    if (state.last_sequence == 0) {
        state.last_sequence = sequence;
        state.repo = repo;
        state.repo_revision = revision;
        state.resync_required = false;
        return CursorResult::Initialized;
    }
    if (sequence == state.last_sequence) return CursorResult::Duplicate;
    if (sequence < state.last_sequence) return CursorResult::Rewind;
    if (sequence != state.last_sequence + 1) {
        state.resync_required = true;
        return CursorResult::Gap;
    }
    state.last_sequence = sequence;
    if (!repo.empty()) state.repo = repo;
    if (!revision.empty()) state.repo_revision = revision;
    return CursorResult::Advanced;
}

} // namespace atperson::protocol
