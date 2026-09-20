#include "../protocol.hpp"

#include <cassert>
#include <filesystem>

using namespace atperson::protocol;

int main() {
    const auto parsed = parse_at_uri("at://did:plc:abc/app.bsky.feed.post/3k1");
    assert(parsed && parsed->did == "did:plc:abc" &&
           parsed->collection == "app.bsky.feed.post" && parsed->rkey == "3k1");
    assert(!parse_at_uri("at://handle.example/app.bsky.feed.post/3k1"));
    assert(!parse_at_uri("at://did:plc:abc/app.bsky.feed.post/3k1?x=1"));
    assert(!parse_at_uri("at://did:plc:abc/Not.bsky.feed/3k1"));
    const auto strong = parse_strong_ref("at://did:plc:abc/app.bsky.feed.post/3k1",
                                         "bafyreiabcdef");
    assert(strong && strong->cid == "bafyreiabcdef");
    assert(!parse_strong_ref("at://did:plc:abc/app.bsky.feed.post/3k1", "bad"));
    const auto blob = parse_blob_ref("bafyreiabcdef", "image/png", 42);
    assert(blob && blob->mime_type == "image/png" && blob->size == 42u);
    assert(!parse_blob_ref("bad", "image/png", 42));
    assert(is_did("did:plc:abc"));
    assert(!is_did("alice.example"));
    assert(is_handle("alice.example"));
    assert(!is_handle("did:plc:abc"));
    const auto identity = accept_identity("did:plc:abc", "alice.example",
                                         "https://plc.directory/did:plc:abc",
                                         "did:key:z6Mkkey", "https://pds.example",
                                         Verification::Verified,
                                         {"did:key:z6Mkrotation"});
    assert(identity && identity->did == "did:plc:abc" && identity->pds_endpoint ==
           "https://pds.example");
    assert(identity->rotation_keys.size() == 1);
    assert(!accept_identity("alice.example", "alice.example", "source", "key",
                            "https://pds.example", Verification::Verified));
    assert(is_nsid("app.bsky.feed.post"));
    assert(!is_nsid("App.Bsky.feed.post"));
    assert(is_cid("bafyreib valid-ish" ) == false);
    assert(is_cid("bafyreiabcdef"));
    assert(is_tid("3jui3s7xq2m2a"));
    assert(!is_tid("not-a-tid"));
    assert(classify_service_role("AtprotoPersonalDataServer") == ServiceRole::Pds);
    assert(classify_service_role("AtprotoRelay") == ServiceRole::Relay);
    assert(classify_service_role("AtprotoAppView") == ServiceRole::AppView);
    assert(classify_service_role("AtprotoFeedGenerator") == ServiceRole::FeedGenerator);
    assert(classify_service_role("AtprotoLabeler") == ServiceRole::Labeler);
    assert(classify_service_role("unknown") == ServiceRole::Unknown);
    assert(authority_for_service(ServiceRole::Pds) == RecordAuthority::Repository);
    assert(authority_for_service(ServiceRole::AppView) == RecordAuthority::AppViewDerived);
    const auto service = accept_service_fact(ServiceRole::AppView,
                                             "https://appview.example",
                                             "did:plc:abc", Verification::Verified);
    assert(service && service->role == ServiceRole::AppView);
    for (const auto role : {ServiceRole::Pds, ServiceRole::Relay,
                            ServiceRole::FeedGenerator, ServiceRole::Labeler}) {
        const auto role_fact = accept_service_fact(role, "https://service.example",
                                                   "did:plc:abc", Verification::Unverified);
        assert(role_fact && role_fact->role == role);
    }
    assert(!accept_service_fact(ServiceRole::Unknown, "https://example", "did:plc:abc",
                                Verification::Verified));
    assert(authority_for_service(ServiceRole::Relay) == RecordAuthority::Unknown);
    const auto repository = accept_repository_fact(
        "did:plc:abc", "3jui3s7xq2m2a", "bafyreiabcdef", "https://pds.example/repo.car",
        Verification::Verified);
    assert(repository && repository->signed_root_cid == "bafyreiabcdef" &&
           repository->verification == Verification::Verified);
    assert(!accept_repository_fact("alice.example", "3jui3s7xq2m2a", "bafyreiabcdef",
                                   "car", Verification::Verified));
    assert(!accept_repository_fact("did:plc:abc", "3jui3s7xq2m2a", "bad", "car",
                                   Verification::Rejected));
    assert(!accept_repository_fact("did:plc:abc", "bafyrev1", "bafyreiabcdef", "car",
                                   Verification::Verified));
    const auto session = accept_oauth_session(
        "https://issuer.example", "did:plc:abc", "repo:* dpop", true,
        Verification::Verified);
    assert(session && session->dpop_bound && session->scope == "repo:* dpop");
    assert(!accept_oauth_session("https://issuer.example", "did:plc:abc", "repo:*",
                                false, Verification::Verified));
    const auto oauth_plan = make_loopback_oauth_plan(
        "http://127.0.0.1:43127/callback", "account:repo?action=manage");
    assert(oauth_plan && oauth_plan->loopback_only &&
           oauth_plan->permission == PermissionKind::RepositoryMigration);
    assert(!make_loopback_oauth_plan("https://evil.example/callback", "repo:*"));
    const auto metadata = localhost_oauth_client_metadata(
        "http://127.0.0.1:43127/callback", "repo:*");
    assert(metadata && metadata->client_id == "http://localhost/" &&
           metadata->dpop_bound);
    const AtUri record_uri{"did:plc:abc", "app.bsky.feed.post", "3k1"};
    assert(reduce_record_observation(record_uri, "bafyreiabcdef", false, false, true).state ==
           RecordState::Present);
    assert(reduce_record_observation(record_uri, "", true, false, true).state ==
           RecordState::Deleted);
    assert(reduce_record_observation(record_uri, "", false, false, false).state ==
           RecordState::Missing);
    assert(reduce_record_observation(record_uri, "bafyreiabcdef", false, true, true).state ==
           RecordState::Unverified);
    assert(classify_sync_event("#commit") == SyncEvent::Commit);
    assert(classify_sync_event("#identity") == SyncEvent::Identity);
    assert(classify_sync_event("#unknown") == SyncEvent::Unknown);
    assert(verification_from_wolfram(true, true) == Verification::Verified);
    assert(verification_from_wolfram(true, false) == Verification::Rejected);
    assert(verification_from_wolfram(false, false) == Verification::Unverified);
    assert(classify_permission("repo:app.bsky.feed.post?action=create") ==
           PermissionKind::Record);
    assert(classify_permission("repo:*") == PermissionKind::AllRecords);
    assert(classify_permission("account:repo?action=manage") ==
           PermissionKind::RepositoryMigration);
    assert(classify_permission("dpop") == PermissionKind::DpopBound);

    assert(classify_xrpc(true, false) == XrpcKind::Query);
    assert(classify_xrpc(false, true) == XrpcKind::Procedure);
    assert(classify_xrpc(false, false, true) == XrpcKind::Subscription);
    assert(classify_xrpc(true, true) == XrpcKind::Unknown);
    const auto lexicon = accept_lexicon_fact(
        "app.bsky.feed.post", XrpcKind::Query, "https://pds.example/lexicon", Verification::Verified);
    assert(lexicon && lexicon->operation == XrpcKind::Query);
    assert(!accept_lexicon_fact("Not.NSID", XrpcKind::Query, "source", Verification::Verified));

    EvidenceStore store;
    ProtocolEvidence fact{EvidenceKind::Identity, "https://pds.example/.well-known",
                          "identity", "did:plc:abc", "did-document-v1", 1, 10,
                          Verification::Unverified, 0.0};
    assert(store.append(fact));
    auto incomplete = fact;
    incomplete.source.clear();
    assert(!store.append(incomplete));
    auto impossible_confidence = fact;
    impossible_confidence.confidence = 1.1;
    assert(!store.append(impossible_confidence));
    assert(!store.append(fact));
    assert(store.entries().size() == 1);
    auto distinct_event = fact;
    distinct_event.event_type = "did-document";
    assert(store.append(distinct_event));
    assert(store.entries().size() == 2);
    auto upgraded = fact;
    upgraded.verification = Verification::Verified;
    assert(store.append(upgraded));
    assert(store.entries().size() == 3);
    const auto replayed = EvidenceStore::replay(
        std::vector<ProtocolEvidence>{fact, fact, fact});
    assert(replayed.entries().size() == 1);

    /* A reconnect may replay the last commit, and a bounded resync may add
     * its repository evidence afterward. Replay must remain deterministic and
     * must not turn either transport event into a second protocol fact. */
    const ProtocolEvidence commit{EvidenceKind::Sync, "wss://relay.example",
                                  "#commit", "did:plc:abc", "rev-a|42", 42,
                                  100, Verification::Unverified, 0.5};
    const ProtocolEvidence reconnect = commit;
    const ProtocolEvidence resync{EvidenceKind::Repository, "https://pds.example",
                                  "#resync", "did:plc:abc", "rev-a|car-root", 43,
                                  101, Verification::Verified, 1.0};
    const std::vector<ProtocolEvidence> replay_input{
        commit, reconnect, resync, commit, resync};
    const auto replay_once = EvidenceStore::replay(replay_input);
    const auto replay_twice = EvidenceStore::replay(replay_input);
    assert(replay_once.entries() == replay_twice.entries());
    assert(replay_once.entries().size() == 2u);
    assert(replay_once.entries()[0].event_type == "#commit");
    assert(replay_once.entries()[1].event_type == "#resync");

    CursorState cursor;
    assert(observe_stream(cursor, 10, "did:plc:abc", "3jui3s7xq2m2a") == CursorResult::Initialized);
    assert(revision_for(cursor, "did:plc:abc") == std::optional<std::string>("3jui3s7xq2m2a"));
    assert(observe_stream(cursor, 10, "did:plc:abc", "3jui3s7xq2m2a") == CursorResult::Duplicate);
    assert(observe_stream(cursor, 12, "did:plc:abc", "3jui3s7xq2m2b") == CursorResult::Gap);
    assert(cursor.resync_required);
    const auto plan = plan_resync(cursor, 0);
    assert(plan.required && plan.repo == "did:plc:abc" && plan.from_sequence == 10 &&
           plan.max_records == 1 && !plan.reason.empty());
    assert(observe_stream(cursor, 11, "did:plc:abc", "3jui3s7xq2m2c") == CursorResult::Gap);
    assert(!complete_resync(cursor, 9, "did:plc:abc", "3jui3s7xq2m2c", Verification::Verified));
    assert(!complete_resync(cursor, 11, "did:plc:abc", "3jui3s7xq2m2c", Verification::Unverified));
    assert(complete_resync(cursor, 11, "did:plc:abc", "3jui3s7xq2m2c", Verification::Verified));
    assert(observe_stream(cursor, 10, "did:plc:abc", "3jui3s7xq2m2c") == CursorResult::Rewind);
    assert(observe_stream(cursor, 12, "did:plc:other", "3jui3s7xq2m2d") == CursorResult::Advanced);
    assert(revision_for(cursor, "did:plc:abc") == std::optional<std::string>("3jui3s7xq2m2c"));
    assert(revision_for(cursor, "did:plc:other") == std::optional<std::string>("3jui3s7xq2m2d"));
    assert(observe_stream(cursor, 13, "did:plc:other", "bad-revision") == CursorResult::Rejected);
    CursorState sequence_only;
    assert(observe_sequence(sequence_only, 20) == CursorResult::Initialized);
    assert(observe_sequence(sequence_only, 22) == CursorResult::Gap);

    const auto path = std::filesystem::temp_directory_path() / "atperson-protocol-evidence-test.bin";
    std::error_code error;
    std::filesystem::remove(path, error);
    EvidenceLedger ledger(path);
    assert(ledger.append(fact));
    assert(!ledger.append(fact));
    assert(append_firehose_event(ledger, "wss://relay.example", "#sync",
                                 "did:plc:abc", "rev-a", 2, 11));
    assert(append_firehose_event(ledger, "wss://relay.example", "#identity",
                                 "did:plc:abc", "alice.example", 3, 12));
    assert(append_identity_fact(ledger, *identity, "https://bsky.social", 3, 12));
    assert(append_service_fact(ledger, *service, "https://bsky.social", 3, 12));
    assert(append_firehose_event(ledger, "wss://relay.example", "#account",
                                 "did:plc:abc", "active", 4, 13));
    assert(append_firehose_event(ledger, "wss://relay.example", "#unknown",
                                 "did:plc:abc", "future", 5, 14));
    assert(append_repository_fact(ledger, *repository, 6, 15));
    assert(append_firehose_event(ledger, "https://pds.example/repo", "#commit",
                                 "did:plc:abc", "invalid-signature", 7, 16,
                                 Verification::Rejected));
    assert(append_firehose_event(ledger, "https://relay.example", "#identity",
                                 "did:plc:unknown", "transport-failure", 8, 17,
                                 Verification::Unverified));
    const auto restored = ledger.entries();
    assert(restored.size() == 10);
    assert(restored[1].event_type == "#sync" &&
           restored[2].event_type == "#identity" &&
           restored[3].payload.find("rotation=did:key:z6Mkrotation") != std::string::npos &&
           restored[4].payload == "role=app-view|endpoint=https://appview.example" &&
           restored[6].event_type == "#unknown" && restored[7].event_type == "#repository" &&
           restored[8].verification == Verification::Rejected &&
           restored[9].verification == Verification::Unverified);
    std::filesystem::remove(path, error);
}
