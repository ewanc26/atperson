#include "protocol.hpp"

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

ServiceRole classify_service_role(std::string_view type) noexcept {
    if (type == "AtprotoPersonalDataServer") return ServiceRole::Pds;
    if (type == "AtprotoRelay") return ServiceRole::Relay;
    if (type == "AtprotoAppView") return ServiceRole::AppView;
    if (type == "AtprotoFeedGenerator") return ServiceRole::FeedGenerator;
    if (type == "AtprotoLabeler") return ServiceRole::Labeler;
    return ServiceRole::Unknown;
}

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

std::optional<StrongRef> parse_strong_ref(std::string_view uri,
                                          std::string_view cid) {
    if (!is_cid(cid)) return std::nullopt;
    const auto parsed = parse_at_uri(uri);
    if (!parsed) return std::nullopt;
    return StrongRef{*parsed, std::string(cid)};
}

bool is_did(std::string_view value) noexcept {
    return value.starts_with("did:") && value.size() > 4 &&
           value.find_first_of(" /?#") == std::string_view::npos;
}

bool is_nsid(std::string_view value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.') return false;
    bool component = false;
    for (const char c : value) {
        if (c == '.') {
            if (!component) return false;
            component = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            component = true;
        } else {
            return false;
        }
    }
    return component;
}

bool is_cid(std::string_view value) noexcept {
    return value.starts_with("b") && value.size() >= 10 &&
           value.find_first_of(" /?#") == std::string_view::npos;
}

bool is_tid(std::string_view value) noexcept {
    if (value.size() != 13) return false;
    return value.find_first_not_of("234567abcdefghijklmnopqrstuvwxyz") == std::string_view::npos;
}

RecordFact reduce_record_observation(AtUri uri, std::string_view cid,
                                     bool explicit_delete, bool from_app_view,
                                     bool observed) {
    RecordFact fact{std::move(uri), std::string(cid), RecordState::Unverified,
                    from_app_view};
    if (!observed) {
        fact.state = RecordState::Missing;
    } else if (explicit_delete) {
        fact.state = RecordState::Deleted;
    } else if (!from_app_view && !cid.empty() && is_cid(cid)) {
        fact.state = RecordState::Present;
    }
    return fact;
}

bool is_handle(std::string_view value) noexcept {
    if (value.empty() || value.starts_with("did:") || value.find('.') == std::string_view::npos) {
        return false;
    }
    return value.find_first_of(" /?#:@") == std::string_view::npos;
}

std::optional<IdentityFact> accept_identity(std::string_view did,
                                            std::string_view handle,
                                            std::string_view source,
                                            std::string_view signing_key,
                                            std::string_view pds_endpoint,
                                            Verification verification,
                                            std::vector<std::string> rotation_keys) {
    if (!is_did(did) || !is_handle(handle) || source.empty() || signing_key.empty() ||
        pds_endpoint.empty() || pds_endpoint.find("https://") != 0) {
        return std::nullopt;
    }
    IdentityFact fact;
    fact.did = did;
    fact.handle = handle;
    fact.did_document_source = source;
    fact.signing_key = signing_key;
    fact.rotation_keys = std::move(rotation_keys);
    fact.pds_endpoint = pds_endpoint;
    fact.verification = verification;
    return fact;
}

SyncEvent classify_sync_event(std::string_view type) noexcept {
    if (type == "#commit") return SyncEvent::Commit;
    if (type == "#sync") return SyncEvent::Sync;
    if (type == "#identity") return SyncEvent::Identity;
    if (type == "#account") return SyncEvent::Account;
    return SyncEvent::Unknown;
}

Verification verification_from_wolfram(bool transport_ok,
                                       bool cryptographically_valid) noexcept {
    if (!transport_ok) return Verification::Unverified;
    return cryptographically_valid ? Verification::Verified : Verification::Rejected;
}

PermissionKind classify_permission(std::string_view scope) noexcept {
    if (scope == "account:repo?action=manage") return PermissionKind::RepositoryMigration;
    if (scope == "repo:*") return PermissionKind::AllRecords;
    if (scope.find("repo:") == 0 && scope.find("action=") != std::string_view::npos) {
        return PermissionKind::Record;
    }
    if (scope == "dpop") return PermissionKind::DpopBound;
    return PermissionKind::Record;
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

EvidenceStore EvidenceStore::replay(const std::vector<ProtocolEvidence> &evidence) {
    EvidenceStore restored;
    for (const auto &entry : evidence) restored.append(entry);
    return restored;
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

ResyncPlan plan_resync(const CursorState &state, std::uint32_t max_records) {
    if (!state.resync_required) return {};
    ResyncPlan plan;
    plan.required = true;
    plan.repo = state.repo;
    plan.from_sequence = state.last_sequence;
    plan.max_records = std::max<std::uint32_t>(1u, max_records);
    plan.reason = "firehose sequence gap requires bounded repository resync";
    return plan;
}

} // namespace atperson::protocol
