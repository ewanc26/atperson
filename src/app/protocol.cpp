#include "atperson/protocol.hpp"

#include <algorithm>

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
