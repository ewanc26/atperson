#ifndef ATPERSON_PROTOCOL_HPP
#define ATPERSON_PROTOCOL_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson::protocol {

enum class Verification { Verified, Unverified, Rejected };
enum class XrpcKind { Query, Procedure, Subscription, Unknown };
enum class EvidenceKind { Identity, Repository, Record, Lexicon, Sync, Authorization };
enum class ServiceRole { Unknown, Pds, Relay, AppView, FeedGenerator, Labeler };
enum class RecordState { Present, Deleted, Missing, Unverified };
enum class RecordAuthority { Repository, AppViewDerived, Unknown };
enum class SyncEvent { Commit, Sync, Identity, Account, Unknown };
enum class PermissionKind { Record, AllRecords, RepositoryMigration, DpopBound };

struct AtUri {
    std::string did;
    std::string collection;
    std::string rkey;
};

struct StrongRef {
    AtUri uri;
    std::string cid;
};

struct BlobRef {
    std::string cid;
    std::string mime_type;
    std::uint64_t size{};
};

struct LexiconFact {
    std::string nsid;
    XrpcKind operation{XrpcKind::Unknown};
    std::string schema_source;
    Verification verification{Verification::Unverified};
};

/* Service identity is supplied by a DID document or Lexicon/service
 * configuration; never infer authority from an endpoint hostname. */
ServiceRole classify_service_role(std::string_view type) noexcept;

struct IdentityFact {
    std::string did;
    std::string handle;
    std::string did_document_source;
    std::string signing_key;
    std::vector<std::string> rotation_keys;
    std::string pds_endpoint;
    Verification verification{Verification::Unverified};
};

std::optional<IdentityFact> accept_identity(std::string_view did,
                                            std::string_view handle,
                                            std::string_view source,
                                            std::string_view signing_key,
                                            std::string_view pds_endpoint,
                                            Verification verification,
                                            std::vector<std::string> rotation_keys = {});

struct ServiceFact {
    ServiceRole role{ServiceRole::Pds};
    std::string endpoint;
    std::string subject_did;
    Verification verification{Verification::Unverified};
};

struct RecordFact {
    AtUri uri;
    std::string cid;
    RecordState state{RecordState::Unverified};
    bool app_view_derived{};
};

RecordAuthority authority_for_service(ServiceRole role) noexcept;

struct AuthorizationFact {
    PermissionKind kind{PermissionKind::Record};
    std::string scope;
    std::string subject_did;
    bool granted{};
    Verification verification{Verification::Unverified};
};

struct OAuthSessionFact {
    std::string issuer;
    std::string subject_did;
    std::string scope;
    bool dpop_bound{};
    Verification verification{Verification::Unverified};
};

struct OAuthAuthorizationPlan {
    std::string redirect_uri;
    std::string scope;
    PermissionKind permission{PermissionKind::Record};
    bool loopback_only{};
};

struct OAuthClientMetadata {
    std::string client_id;
    std::string redirect_uri;
    std::string scope;
    bool dpop_bound{true};
};

std::optional<OAuthAuthorizationPlan> make_loopback_oauth_plan(
    std::string_view redirect_uri, std::string_view scope);
std::optional<OAuthClientMetadata> localhost_oauth_client_metadata(
    std::string_view redirect_uri, std::string_view scope);

std::optional<OAuthSessionFact> accept_oauth_session(
    std::string_view issuer, std::string_view subject_did,
    std::string_view scope, bool dpop_bound, Verification verification);

struct RepositoryFact {
    std::string repo_did;
    std::string revision;
    std::string signed_root_cid;
    std::string car_source;
    Verification verification{Verification::Unverified};
};

std::optional<RepositoryFact> accept_repository_fact(
    std::string_view repo_did, std::string_view revision,
    std::string_view signed_root_cid, std::string_view car_source,
    Verification verification);

/* Parse only the authority/collection/rkey form; query and fragment suffixes
 * are rejected so callers cannot accidentally learn from an ambiguous URI. */
std::optional<AtUri> parse_at_uri(std::string_view value);
std::optional<StrongRef> parse_strong_ref(std::string_view uri,
                                          std::string_view cid);
std::optional<BlobRef> parse_blob_ref(std::string_view cid, std::string_view mime_type,
                                      std::uint64_t size);
std::optional<LexiconFact> accept_lexicon_fact(std::string_view nsid, XrpcKind operation,
                                               std::string_view schema_source,
                                               Verification verification);
bool is_nsid(std::string_view value) noexcept;
bool is_cid(std::string_view value) noexcept;
bool is_tid(std::string_view value) noexcept;
RecordFact reduce_record_observation(AtUri uri, std::string_view cid,
                                     bool explicit_delete, bool from_app_view,
                                     bool observed);
bool is_did(std::string_view value) noexcept;
bool is_handle(std::string_view value) noexcept;
SyncEvent classify_sync_event(std::string_view type) noexcept;
/* Maps Wolfram's transport/crypto outcome without performing verification in
 * atperson. A transport failure is retained as unverified evidence; a parsed
 * but invalid signature is retained as rejected evidence. */
Verification verification_from_wolfram(bool transport_ok,
                                       bool cryptographically_valid) noexcept;
PermissionKind classify_permission(std::string_view scope) noexcept;

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
    bool operator==(const ProtocolEvidence &) const = default;
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
    [[nodiscard]] bool contains(std::string_view source, std::string_view payload,
                                Verification verification) const;
    static EvidenceStore replay(const std::vector<ProtocolEvidence> &evidence);

  private:
    std::vector<ProtocolEvidence> entries_;
};

/* Append-only protocol evidence generation. This file is intentionally
 * independent from the social observation ledger and model snapshot. */
class EvidenceLedger {
  public:
    explicit EvidenceLedger(const std::filesystem::path &path);
    ~EvidenceLedger();
    EvidenceLedger(const EvidenceLedger &) = delete;
    EvidenceLedger &operator=(const EvidenceLedger &) = delete;
    bool append(ProtocolEvidence evidence);
    [[nodiscard]] std::vector<ProtocolEvidence> entries() const;

  private:
    std::filesystem::path path_;
};

/* Record any firehose event family without requiring the social-content
 * reducer to understand it. Unknown families are retained as unverified
 * evidence so newer protocol events remain inspectable. */
bool append_firehose_event(EvidenceLedger &ledger, std::string_view source,
                           std::string_view event_type, std::string_view subject,
                           std::string_view payload, std::uint64_t sequence,
                           std::uint64_t observed_at,
                           Verification verification = Verification::Unverified);
bool append_repository_fact(EvidenceLedger &ledger, const RepositoryFact &fact,
                            std::uint64_t sequence, std::uint64_t observed_at);

struct CursorState {
    std::uint64_t last_sequence{};
    std::string repo;
    std::string repo_revision;
    struct RepositoryRevision {
        std::string repo;
        std::string revision;
        bool operator==(const RepositoryRevision &) const = default;
    };
    std::vector<RepositoryRevision> repository_revisions;
    bool resync_required{};
};

enum class CursorResult { Initialized, Advanced, Duplicate, Gap, Rewind, Rejected };
CursorResult observe_sequence(CursorState &state, std::uint64_t sequence);
CursorResult observe_stream(CursorState &state, std::uint64_t sequence,
                            std::string_view repo, std::string_view revision);
std::optional<std::string> revision_for(const CursorState &state,
                                        std::string_view repo);

struct ResyncPlan {
    bool required{};
    std::string repo;
    std::uint64_t from_sequence{};
    std::uint32_t max_records{};
    std::string reason;
};

/* A gap never becomes an unbounded catch-up loop. The caller hands this plan
 * to Wolfram's repository/CAR synchronizer and keeps the stream paused until
 * the bounded snapshot has been validated. */
ResyncPlan plan_resync(const CursorState &state, std::uint32_t max_records);

/* Commit a validated bounded repository resynchronization. Stream events
 * remain paused while resync_required is true; only the caller that has
 * validated the repository revision and applied its bounded CAR may clear it. */
bool complete_resync(CursorState &state, std::uint64_t sequence,
                     std::string_view repo, std::string_view revision,
                     Verification verification);

} // namespace atperson::protocol

#endif
