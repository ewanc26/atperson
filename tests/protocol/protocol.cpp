#include "atperson/protocol.hpp"

#include <cassert>
#include <filesystem>

using namespace atperson::protocol;

int main() {
    const auto parsed = parse_at_uri("at://did:plc:abc/app.bsky.feed.post/3k1");
    assert(parsed && parsed->did == "did:plc:abc" &&
           parsed->collection == "app.bsky.feed.post" && parsed->rkey == "3k1");
    assert(!parse_at_uri("at://handle.example/app.bsky.feed.post/3k1"));
    assert(!parse_at_uri("at://did:plc:abc/app.bsky.feed.post/3k1?x=1"));
    assert(is_did("did:plc:abc"));
    assert(!is_did("alice.example"));
    assert(is_handle("alice.example"));
    assert(!is_handle("did:plc:abc"));

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
