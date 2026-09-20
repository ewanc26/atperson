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
    const auto strong = parse_strong_ref("at://did:plc:abc/app.bsky.feed.post/3k1",
                                         "bafyreiabcdef");
    assert(strong && strong->cid == "bafyreiabcdef");
    assert(!parse_strong_ref("at://did:plc:abc/app.bsky.feed.post/3k1", "bad"));
    assert(is_did("did:plc:abc"));
    assert(!is_did("alice.example"));
    assert(is_handle("alice.example"));
    assert(!is_handle("did:plc:abc"));
    const auto identity = accept_identity("did:plc:abc", "alice.example",
                                         "https://plc.directory/did:plc:abc",
                                         "did:key:z6Mkkey", "https://pds.example",
                                         Verification::Verified);
    assert(identity && identity->did == "did:plc:abc" && identity->pds_endpoint ==
           "https://pds.example");
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

    EvidenceStore store;
    ProtocolEvidence fact{EvidenceKind::Identity, "https://pds.example/.well-known",
                          "identity", "did:plc:abc", "did-document-v1", 1, 10,
                          Verification::Verified, 1.0};
    assert(store.append(fact));
    assert(!store.append(fact));
    assert(store.entries().size() == 1);
    const auto replayed = EvidenceStore::replay(
        std::vector<ProtocolEvidence>{fact, fact, fact});
    assert(replayed.entries().size() == 1);

    CursorState cursor;
    assert(observe_stream(cursor, 10, "did:plc:abc", "rev-a") == CursorResult::Initialized);
    assert(observe_stream(cursor, 10, "did:plc:abc", "rev-a") == CursorResult::Duplicate);
    assert(observe_stream(cursor, 12, "did:plc:abc", "rev-c") == CursorResult::Gap);
    assert(cursor.resync_required);
    const auto plan = plan_resync(cursor, 0);
    assert(plan.required && plan.repo == "did:plc:abc" && plan.from_sequence == 10 &&
           plan.max_records == 1 && !plan.reason.empty());
    assert(observe_stream(cursor, 9, "did:plc:abc", "rev-b") == CursorResult::Rewind);

    const auto path = std::filesystem::temp_directory_path() / "atperson-protocol-evidence-test.bin";
    std::error_code error;
    std::filesystem::remove(path, error);
    EvidenceLedger ledger(path);
    assert(ledger.append(fact));
    assert(!ledger.append(fact));
    const auto restored = ledger.entries();
    assert(restored.size() == 1 && restored[0].subject == "did:plc:abc");
    std::filesystem::remove(path, error);
}
