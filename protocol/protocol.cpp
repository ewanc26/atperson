#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

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

namespace {
const char *service_role_name(ServiceRole role) noexcept {
    switch (role) {
    case ServiceRole::Pds: return "pds";
    case ServiceRole::Relay: return "relay";
    case ServiceRole::AppView: return "app-view";
    case ServiceRole::FeedGenerator: return "feed-generator";
    case ServiceRole::Labeler: return "labeler";
    case ServiceRole::Unknown: return "unknown";
    }
    return "unknown";
}

void remember_revision(CursorState &state, std::string_view repo,
                       std::string_view revision) {
    if (repo.empty() || revision.empty()) return;
    auto found = std::find_if(state.repository_revisions.begin(),
                              state.repository_revisions.end(),
                              [&](const auto &item) { return item.repo == repo; });
    if (found == state.repository_revisions.end()) {
        state.repository_revisions.push_back({std::string(repo), std::string(revision)});
    } else {
        found->revision = revision;
    }
}
} // namespace

ServiceRole classify_service_role(std::string_view type) noexcept {
    if (type == "AtprotoPersonalDataServer") return ServiceRole::Pds;
    if (type == "AtprotoRelay") return ServiceRole::Relay;
    if (type == "AtprotoAppView") return ServiceRole::AppView;
    if (type == "AtprotoFeedGenerator") return ServiceRole::FeedGenerator;
    if (type == "AtprotoLabeler") return ServiceRole::Labeler;
    return ServiceRole::Unknown;
}

RecordAuthority authority_for_service(ServiceRole role) noexcept {
    if (role == ServiceRole::AppView) return RecordAuthority::AppViewDerived;
    if (role == ServiceRole::Pds) return RecordAuthority::Repository;
    return RecordAuthority::Unknown;
}

std::optional<ServiceFact> accept_service_fact(ServiceRole role,
                                               std::string_view endpoint,
                                               std::string_view subject_did,
                                               Verification verification) {
    if (role == ServiceRole::Unknown || endpoint.find("https://") != 0 ||
        !is_did(subject_did)) {
        return std::nullopt;
    }
    return ServiceFact{role, std::string(endpoint), std::string(subject_did), verification};
}

std::optional<RepositoryFact> accept_repository_fact(
    std::string_view repo_did, std::string_view revision,
    std::string_view signed_root_cid, std::string_view car_source,
    Verification verification) {
    if (!is_did(repo_did) || !is_tid(revision) || !is_cid(signed_root_cid) ||
        car_source.empty()) {
        return std::nullopt;
    }
    return RepositoryFact{std::string(repo_did), std::string(revision),
                          std::string(signed_root_cid), std::string(car_source),
                          verification};
}

std::optional<OAuthSessionFact> accept_oauth_session(
    std::string_view issuer, std::string_view subject_did,
    std::string_view scope, bool dpop_bound, Verification verification) {
    if (issuer.empty() || !is_did(subject_did) || scope.empty() || !dpop_bound) {
        return std::nullopt;
    }
    return OAuthSessionFact{std::string(issuer), std::string(subject_did),
                            std::string(scope), dpop_bound, verification};
}

std::optional<OAuthAuthorizationPlan> make_loopback_oauth_plan(
    std::string_view redirect_uri, std::string_view scope) {
    if (redirect_uri.find("http://127.0.0.1:") != 0 || scope.empty() ||
        redirect_uri.find_first_of("?#") != std::string_view::npos) {
        return std::nullopt;
    }
    const auto port_start = std::string_view("http://127.0.0.1:").size();
    const auto slash = redirect_uri.find('/', port_start);
    const auto port = redirect_uri.substr(
        port_start, slash == std::string_view::npos ? std::string_view::npos
                                                     : slash - port_start);
    if (port.empty() || port.find_first_not_of("0123456789") !=
                            std::string_view::npos) {
        return std::nullopt;
    }
    return OAuthAuthorizationPlan{std::string(redirect_uri), std::string(scope),
                                  classify_permission(scope), true};
}

std::optional<OAuthClientMetadata> localhost_oauth_client_metadata(
    std::string_view redirect_uri, std::string_view scope) {
    if (!make_loopback_oauth_plan(redirect_uri, scope)) return std::nullopt;
    return OAuthClientMetadata{"http://localhost/", std::string(redirect_uri),
                               std::string(scope), true};
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
    if (!out.did.starts_with("did:") || !is_nsid(out.collection) ||
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

std::optional<BlobRef> parse_blob_ref(std::string_view cid, std::string_view mime_type,
                                      std::uint64_t size) {
    if (!is_cid(cid) || mime_type.empty() || mime_type.find('/') == std::string_view::npos) {
        return std::nullopt;
    }
    return BlobRef{std::string(cid), std::string(mime_type), size};
}

std::optional<LexiconFact> accept_lexicon_fact(std::string_view nsid, XrpcKind operation,
                                               std::string_view schema_source,
                                               Verification verification) {
    if (!is_nsid(nsid) || operation == XrpcKind::Unknown || schema_source.empty()) {
        return std::nullopt;
    }
    return LexiconFact{std::string(nsid), operation, std::string(schema_source), verification};
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

bool EvidenceStore::contains(std::string_view source, std::string_view payload,
                             Verification verification) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const auto &entry) {
        return entry.source == source && entry.payload == payload &&
               entry.verification == verification;
    });
}

bool EvidenceStore::contains(const ProtocolEvidence &evidence) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const auto &entry) {
        return entry.kind == evidence.kind && entry.source == evidence.source &&
               entry.event_type == evidence.event_type &&
               entry.subject == evidence.subject && entry.payload == evidence.payload &&
               entry.verification == evidence.verification;
    });
}

bool EvidenceStore::append(ProtocolEvidence evidence) {
    if (evidence.source.empty() || evidence.event_type.empty() ||
        evidence.subject.empty() || evidence.payload.empty() ||
        (evidence.sequence == 0 && evidence.observed_at == 0) ||
        !std::isfinite(evidence.confidence) || evidence.confidence < 0.0 ||
        evidence.confidence > 1.0) {
        return false;
    }
    if (contains(evidence)) return false;
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
    if (const auto parent = path_.parent_path(); !parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) throw std::runtime_error("create protocol evidence directory");
    }
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
    out.flush();
    if (!out) throw std::runtime_error("write protocol evidence ledger");
#if !defined(_WIN32)
    const int fd = ::open(path_.c_str(), O_WRONLY);
    if (fd >= 0) {
        if (::fsync(fd) != 0) {
            ::close(fd);
            throw std::runtime_error("sync protocol evidence ledger");
        }
        ::close(fd);
    }
#endif
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

bool append_firehose_event(EvidenceLedger &ledger, std::string_view source,
                           std::string_view event_type, std::string_view subject,
                           std::string_view payload, std::uint64_t sequence,
                           std::uint64_t observed_at, Verification verification) {
    ProtocolEvidence evidence{EvidenceKind::Sync, std::string(source),
                               std::string(event_type), std::string(subject),
                               std::string(payload), sequence, observed_at,
                               verification, verification == Verification::Verified ? 1.0 : 0.0};
    return ledger.append(std::move(evidence));
}

bool append_identity_fact(EvidenceLedger &ledger, const IdentityFact &fact,
                          std::string_view source, std::uint64_t sequence,
                          std::uint64_t observed_at) {
    if (!is_did(fact.did) || !is_handle(fact.handle) || source.empty()) return false;
    std::string payload = fact.handle + "|" + fact.pds_endpoint + "|" +
                          fact.signing_key;
    for (const auto &rotation : fact.rotation_keys) payload += "|rotation=" + rotation;
    return ledger.append({EvidenceKind::Identity, std::string(source), "#identity",
                          fact.did, std::move(payload), sequence, observed_at,
                          fact.verification, fact.verification == Verification::Verified ? 1.0 : 0.5});
}

bool append_service_fact(EvidenceLedger &ledger, const ServiceFact &fact,
                         std::string_view source, std::uint64_t sequence,
                         std::uint64_t observed_at) {
    if (fact.role == ServiceRole::Unknown || source.empty() ||
        !is_did(fact.subject_did) || fact.endpoint.find("https://") != 0) {
        return false;
    }
    const std::string payload = "role=" + std::string(service_role_name(fact.role)) +
                                "|endpoint=" + fact.endpoint;
    return ledger.append({EvidenceKind::Identity, std::string(source), "#service",
                          fact.subject_did, payload, sequence, observed_at,
                          fact.verification, fact.verification == Verification::Verified ? 1.0 : 0.5});
}

bool append_repository_fact(EvidenceLedger &ledger, const RepositoryFact &fact,
                            std::uint64_t sequence, std::uint64_t observed_at) {
    return append_firehose_event(
        ledger, fact.car_source, "#repository", fact.repo_did,
        fact.revision + "|" + fact.signed_root_cid, sequence, observed_at,
        fact.verification);
}

CursorResult observe_sequence(CursorState &state, std::uint64_t sequence) {
    if (sequence == 0u) return CursorResult::Rejected;
    if (state.last_sequence == 0) {
        state.last_sequence = sequence;
        state.resync_required = false;
        return CursorResult::Initialized;
    }
    if (state.resync_required) return CursorResult::Gap;
    if (sequence == state.last_sequence) return CursorResult::Duplicate;
    if (sequence < state.last_sequence) return CursorResult::Rewind;
    if (sequence != state.last_sequence + 1) {
        state.resync_required = true;
        return CursorResult::Gap;
    }
    state.last_sequence = sequence;
    return CursorResult::Advanced;
}

CursorResult observe_stream(CursorState &state, std::uint64_t sequence,
                            std::string_view repo, std::string_view revision) {
    if (!is_did(repo) || !is_tid(revision)) return CursorResult::Rejected;
    const auto result = observe_sequence(state, sequence);
    if (result != CursorResult::Initialized && result != CursorResult::Advanced)
        return result;
    if (result == CursorResult::Initialized) {
        state.repo = repo;
        state.repo_revision = revision;
        remember_revision(state, repo, revision);
        return result;
    }
    if (!repo.empty()) state.repo = repo;
    if (!revision.empty()) {
        state.repo_revision = revision;
        remember_revision(state, repo, revision);
    }
    return CursorResult::Advanced;
}

std::optional<std::string> revision_for(const CursorState &state,
                                        std::string_view repo) {
    const auto found = std::find_if(
        state.repository_revisions.begin(), state.repository_revisions.end(),
        [&](const auto &item) { return item.repo == repo; });
    if (found == state.repository_revisions.end()) return std::nullopt;
    return found->revision;
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

bool complete_resync(CursorState &state, std::uint64_t sequence,
                     std::string_view repo, std::string_view revision,
                     Verification verification) {
    if (!state.resync_required || sequence < state.last_sequence || repo.empty() ||
        revision.empty() || !is_did(repo) || !is_tid(revision) ||
        verification != Verification::Verified) return false;
    state.last_sequence = sequence;
    state.repo = repo;
    state.repo_revision = revision;
    remember_revision(state, repo, revision);
    state.resync_required = false;
    return true;
}

} // namespace atperson::protocol
