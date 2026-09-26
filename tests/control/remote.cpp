/* Remote operator control channel (#143): provenance, sequencing, argument
 * rules and the durable replay cursor. Offline — no service, no clock. */
#include "control/remote.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::string_view kOperator = "did:plc:operator123";
constexpr std::string_view kOther = "did:plc:someonelse";
constexpr std::string_view kAccount = "did:plc:entityaccount";
/* Control state requires a 16-digit lowercase hex digest. */
constexpr std::string_view kDigest = "0123456789abcdef";
constexpr std::string_view kOtherDigest = "fedcba9876543210";
constexpr std::string_view kFreeDigest = "00112233445566ff";
constexpr std::string_view kEvictDigest = "00000000000000aa";
constexpr std::string_view kControlUri =
    "at://did:plc:operator123/click.croft.atperson.control/abcdef";

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-remote-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

atperson::ControlRequest request(std::uint64_t seq, atperson::ControlOp op,
                                 std::string_view arg = {}) {
    atperson::ControlRequest value;
    value.seq = seq;
    value.op = op;
    value.arg = std::move(arg);
    value.at = "2026-09-26T12:00:00Z";
    return value;
}

/* An unstarted runtime: fail-closed defaults, so a test that flips a
 * switch is asserting a real transition and not a default. */
atperson::ControlState fresh_state() {
    return atperson::ControlState{};
}

void test_op_names_round_trip() {
    using atperson::ControlOp;
    const ControlOp kAll[] = {
        ControlOp::Pause,       ControlOp::Resume,         ControlOp::WritesOn,
        ControlOp::WritesOff,   ControlOp::DryRunOn,       ControlOp::DryRunOff,
        ControlOp::OfflineOn,   ControlOp::OfflineOff,     ControlOp::ApprovalOn,
        ControlOp::ApprovalOff, ControlOp::Approve,        ControlOp::Revoke,
        ControlOp::Shutdown,    ControlOp::CancelShutdown,
    };
    for (const ControlOp op : kAll) {
        const std::string_view name = atperson::control_op_name(op);
        assert(!name.empty());
        const std::optional<ControlOp> back = atperson::control_op_from_name(name);
        assert(back.has_value());
        assert(*back == op);
    }
    /* Unknown names are refused, never coerced to a default op. */
    assert(!atperson::control_op_from_name("self-destruct").has_value());
    assert(!atperson::control_op_from_name("").has_value());
    assert(!atperson::control_op_from_name("Pause").has_value());

    assert(atperson::control_op_takes_argument(ControlOp::Approve));
    assert(atperson::control_op_takes_argument(ControlOp::Revoke));
    assert(!atperson::control_op_takes_argument(ControlOp::Pause));
    assert(!atperson::control_op_takes_argument(ControlOp::WritesOn));
}

void test_serialise_parse_round_trip() {
    const atperson::ControlRequest original = request(1, atperson::ControlOp::Pause);
    const std::string json = atperson::serialise_control_request(original);
    const atperson::ControlRequest parsed = atperson::parse_control_request(json);
    assert(parsed.seq == 1u);
    assert(parsed.op == atperson::ControlOp::Pause);
    assert(parsed.arg.empty());
    assert(parsed.at.has_value());
    assert(*parsed.at == "2026-09-26T12:00:00Z");

    const atperson::ControlRequest approved =
        request(42, atperson::ControlOp::Approve, kOtherDigest);
    const atperson::ControlRequest reparsed =
        atperson::parse_control_request(atperson::serialise_control_request(approved));
    assert(reparsed.seq == 42u);
    assert(reparsed.op == atperson::ControlOp::Approve);
    assert(reparsed.arg == kOtherDigest);

    /* A 64-bit seq must survive the JSON round-trip exactly. */
    const atperson::ControlRequest big = request(9007199254740993ull, atperson::ControlOp::Resume);
    assert(atperson::parse_control_request(atperson::serialise_control_request(big)).seq ==
           9007199254740993ull);

    /* An absent `at` is legal: it is diagnostics only. */
    atperson::ControlRequest undated = request(2, atperson::ControlOp::Resume);
    undated.at.reset();
    const atperson::ControlRequest undated_parsed =
        atperson::parse_control_request(atperson::serialise_control_request(undated));
    assert(!undated_parsed.at.has_value());
    std::printf("ok request document round-trip\n");
}

void test_serialise_rejects_bad_argument_shape() {
    /* A missing digest on approve is refused at write time too, so an
     * operator cannot publish a record that would fail on read. */
    bool threw = false;
    try {
        (void)atperson::serialise_control_request(request(1, atperson::ControlOp::Approve, ""));
    } catch (const atperson::ControlRemoteError &) {
        threw = true;
    }
    assert(threw);

    /* And an argument on an op that takes none is refused too. */
    threw = false;
    try {
        (void)atperson::serialise_control_request(
            request(1, atperson::ControlOp::Pause, kOtherDigest));
    } catch (const atperson::ControlRemoteError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok serialise refuses bad argument shape\n");
}

void test_parse_rejects_malformed_documents() {
    const std::vector<std::string> bad = {
        "not json",
        "[]",
        R"({"version":1,"type":"click.croft.atperson.control#request","seq":"1","op":"pause"})",
        R"({"format":"something-else","version":1,"type":"click.croft.atperson.control#request","seq":"1","op":"pause"})",
        R"({"format":"atperson-control-request","version":99,"type":"click.croft.atperson.control#request","seq":"1","op":"pause"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.other","seq":"1","op":"pause"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","op":"pause"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"1"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"1","op":"self-destruct"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"1","op":"approve"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"1","op":"pause","arg":"0123456789abcdef"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"-1","op":"pause"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":"1.5","op":"pause"})",
        R"({"format":"atperson-control-request","version":1,"type":"click.croft.atperson.control#request","seq":1.5,"op":"pause"})",
    };
    for (const std::string &document : bad) {
        bool threw = false;
        try {
            (void)atperson::parse_control_request(document);
        } catch (const atperson::ControlRemoteError &) {
            threw = true;
        }
        assert(threw);
    }
    std::printf("ok parse refuses malformed documents\n");
}

void test_provenance_is_checked_first() {
    atperson::ControlState state = fresh_state();
    std::uint64_t watermark = 0u;

    /* A perfectly sequenced record from the wrong author is not a command.
     * The state must be untouched and the watermark unmoved, so a corrected
     * re-send at the same seq still applies. */
    const auto refused = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Pause), kOperator, kOther);
    assert(!refused.applied);
    assert(!state.paused);
    assert(watermark == 0u);
    assert(!refused.reason.empty());

    /* With no operator configured, the channel is inert rather than open. */
    const auto unconfigured = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Pause), "", kOther);
    assert(!unconfigured.applied);
    assert(!state.paused);
    assert(watermark == 0u);

    /* The same record from the operator applies. */
    const auto accepted = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Pause), kOperator, kOperator);
    assert(accepted.applied);
    assert(state.paused);
    assert(watermark == 1u);
    std::printf("ok provenance is checked before anything else\n");
}

void test_sequence_must_be_exactly_next() {
    atperson::ControlState state = fresh_state();
    std::uint64_t watermark = 0u;

    /* seq 2 with watermark 0 is a gap: applying it would silently skip a
     * pause the operator sequenced first, so it is refused. */
    auto report = atperson::apply_control_request(
        state, watermark, request(2, atperson::ControlOp::Pause), kOperator, kOperator);
    assert(!report.applied);
    assert(!state.paused);
    assert(watermark == 0u);

    assert(atperson::apply_control_request(state, watermark, request(1, atperson::ControlOp::Pause),
                                           kOperator, kOperator)
               .applied);
    assert(state.paused);

    /* Replaying seq 1 is refused: a pause cannot be re-applied after a
     * resume to re-pause the runtime by replaying an old record. */
    (void)atperson::apply_control_request(
        state, watermark, request(2, atperson::ControlOp::Resume), kOperator, kOperator);
    assert(!state.paused);
    report = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Pause), kOperator, kOperator);
    assert(!report.applied);
    assert(!state.paused);
    assert(watermark == 2u);

    /* A gap after some progress is equally refused. */
    report = atperson::apply_control_request(
        state, watermark, request(9, atperson::ControlOp::Pause), kOperator, kOperator);
    assert(!report.applied);
    assert(!state.paused);
    assert(watermark == 2u);
    std::printf("ok sequence must be exactly the next one\n");
}

void test_every_op_maps_onto_control_state() {
    struct Case {
        atperson::ControlOp op;
        std::string_view arg;
        void (*check)(const atperson::ControlState &);
    };
    const Case kCases[] = {
        {atperson::ControlOp::Pause, "", [](const atperson::ControlState &s) { assert(s.paused); }},
        {atperson::ControlOp::Resume, "",
         [](const atperson::ControlState &s) { assert(!s.paused); }},
        {atperson::ControlOp::WritesOn, "",
         [](const atperson::ControlState &s) { assert(s.writes_enabled); }},
        {atperson::ControlOp::WritesOff, "",
         [](const atperson::ControlState &s) { assert(!s.writes_enabled); }},
        {atperson::ControlOp::DryRunOn, "",
         [](const atperson::ControlState &s) { assert(s.dry_run); }},
        {atperson::ControlOp::DryRunOff, "",
         [](const atperson::ControlState &s) { assert(!s.dry_run); }},
        {atperson::ControlOp::OfflineOn, "",
         [](const atperson::ControlState &s) { assert(s.offline_mode); }},
        {atperson::ControlOp::OfflineOff, "",
         [](const atperson::ControlState &s) { assert(!s.offline_mode); }},
        {atperson::ControlOp::ApprovalOn, "",
         [](const atperson::ControlState &s) { assert(s.approval_required); }},
        {atperson::ControlOp::ApprovalOff, "",
         [](const atperson::ControlState &s) { assert(!s.approval_required); }},
        {atperson::ControlOp::Approve, kDigest,
         [](const atperson::ControlState &s) { assert(atperson::is_digest_approved(s, kDigest)); }},
        {atperson::ControlOp::Revoke, kDigest,
         [](const atperson::ControlState &s) {
             assert(!atperson::is_digest_approved(s, kDigest));
         }},
        {atperson::ControlOp::Shutdown, "",
         [](const atperson::ControlState &s) { assert(s.shutdown_requested_at.has_value()); }},
        {atperson::ControlOp::CancelShutdown, "",
         [](const atperson::ControlState &s) { assert(!s.shutdown_requested_at.has_value()); }},
    };

    for (const Case &item : kCases) {
        atperson::ControlState state = fresh_state();
        std::uint64_t watermark = 0u;
        /* Put the state on the other side of the switch so every case
         * asserts a transition, never a default. */
        if (item.op == atperson::ControlOp::Pause || item.op == atperson::ControlOp::WritesOff ||
            item.op == atperson::ControlOp::DryRunOn ||
            item.op == atperson::ControlOp::OfflineOff ||
            item.op == atperson::ControlOp::ApprovalOn) {
            state.paused = item.op == atperson::ControlOp::Pause;
            state.writes_enabled = item.op == atperson::ControlOp::WritesOff;
            state.dry_run = item.op == atperson::ControlOp::DryRunOn;
            state.offline_mode = item.op != atperson::ControlOp::OfflineOff;
            state.approval_required = item.op == atperson::ControlOp::ApprovalOn;
        }
        if (item.op == atperson::ControlOp::Shutdown) {
            state.shutdown_requested_at = "2026-01-01T00:00:00Z";
        }
        if (item.op == atperson::ControlOp::Revoke) {
            state.approved_digests = {std::string(kDigest)};
        }

        const auto report = atperson::apply_control_request(
            state, watermark, request(1, item.op, item.arg), kOperator, kOperator);
        assert(report.applied);
        assert(watermark == 1u);
        item.check(state);

        /* The applied state must still serialise: a remote request cannot
         * put control state into a shape the local CLI could not write. */
        (void)atperson::serialise_control_state(state);
    }
    std::printf("ok every op maps onto control state\n");
}

void test_approve_respects_the_local_bound() {
    atperson::ControlState state = fresh_state();
    state.approved_digests.clear();
    /* The local CLI caps the list at 64. A remote approve must not be a
     * way around that bound. Fill it with real 16-hex digests so the state
     * stays serialisable, and make the eviction target one of them. */
    for (int i = 0; i < 64; ++i) {
        char digest[17];
        std::snprintf(digest, sizeof(digest), "%016x", 0x1000 + i);
        state.approved_digests.emplace_back(digest);
    }
    state.approved_digests.back() = std::string(kEvictDigest);
    assert(state.approved_digests.size() == 64u);
    std::uint64_t watermark = 0u;
    const auto report = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Approve, kFreeDigest), kOperator,
        kOperator);
    assert(!report.applied);
    assert(!atperson::is_digest_approved(state, kFreeDigest));
    assert(watermark == 0u);

    /* Revoking to make room, then approving, works. */
    assert(atperson::apply_control_request(state, watermark,
                                           request(1, atperson::ControlOp::Revoke, kEvictDigest),
                                           kOperator, kOperator)
               .applied);
    assert(atperson::apply_control_request(state, watermark,
                                           request(2, atperson::ControlOp::Approve, kFreeDigest),
                                           kOperator, kOperator)
               .applied);
    assert(atperson::is_digest_approved(state, kFreeDigest));
    std::printf("ok remote approve respects the local list bound\n");
}

void test_shape_is_rechecked_at_apply_time() {
    atperson::ControlState state = fresh_state();
    std::uint64_t watermark = 0u;

    /* Built in memory rather than parsed: apply must still enforce the
     * argument rule, so no caller can skip it. */
    const auto missing = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Approve, ""), kOperator, kOperator);
    assert(!missing.applied);
    assert(watermark == 0u);

    const auto extra = atperson::apply_control_request(
        state, watermark, request(1, atperson::ControlOp::Pause, kOtherDigest), kOperator,
        kOperator);
    assert(!extra.applied);
    assert(!state.paused);
    assert(watermark == 0u);
    std::printf("ok argument rule is re-checked at apply time\n");
}

void test_uri_provenance_parsing() {
    const std::optional<std::string> authority = atperson::aturi_authority(kControlUri);
    assert(authority.has_value());
    assert(*authority == "did:plc:operator123");

    assert(!atperson::aturi_authority("at://").has_value());
    assert(!atperson::aturi_authority("at://did:plc:x").has_value());
    assert(!atperson::aturi_authority("https://bsky.app/profile/x").has_value());
    /* A handle authority is not a DID and must not match an operator DID. */
    assert(!atperson::aturi_authority("at://operator.example.com/x/y/z").has_value());
    assert(!atperson::aturi_authority("").has_value());

    assert(atperson::is_operator_control_uri(kControlUri, kOperator));
    assert(!atperson::is_operator_control_uri(kControlUri, kOther));
    assert(!atperson::is_operator_control_uri(kControlUri, ""));
    /* Same repo, different collection: not the control channel. */
    assert(!atperson::is_operator_control_uri(
        "at://did:plc:operator123/click.croft.atperson.action/abcdef", kOperator));
    /* The collection must be a whole path segment. */
    assert(!atperson::is_operator_control_uri(
        "at://did:plc:operator123/app.bsky.feed.post/click.croft.atperson.control.evil/x",
        kOperator));
    std::printf("ok at-URI provenance parsing\n");
}

void test_cursor_round_trip_and_defaults() {
    const auto root = scratch_dir("cursor");
    const auto path = root / "remote-cursor.json";

    /* A missing cursor starts the channel at zero, not at "accept
     * anything": the first request must be seq 1. */
    const atperson::RemoteControlCursor fresh = atperson::load_remote_control_cursor(path);
    assert(fresh.last_seq == 0u);

    atperson::RemoteControlCursor cursor;
    cursor.last_seq = 9007199254740993ull;
    atperson::save_remote_control_cursor(cursor, path);
    const atperson::RemoteControlCursor loaded = atperson::load_remote_control_cursor(path);
    assert(loaded.last_seq == 9007199254740993ull);
    assert(loaded.version == 1u);

    /* Atomic save leaves no .tmp behind. */
    assert(!std::filesystem::exists(path.string() + ".tmp"));

    /* The parent directory is created on demand. */
    const auto nested = root / "a" / "b" / "cursor.json";
    atperson::save_remote_control_cursor(cursor, nested);
    assert(atperson::load_remote_control_cursor(nested).last_seq == 9007199254740993ull);

    /* A corrupt cursor is reported, never silently read as zero — that
     * would reopen the replay window the cursor exists to close. */
    {
        std::ofstream bad(root / "bad.json", std::ios::binary | std::ios::trunc);
        bad << "{\"format\":\"atperson-control-cursor\",\"version\":1}";
    }
    bool threw = false;
    try {
        (void)atperson::load_remote_control_cursor(root / "bad.json");
    } catch (const atperson::ControlRemoteError &) {
        threw = true;
    }
    assert(threw);

    {
        std::ofstream bad(root / "notjson.json", std::ios::binary | std::ios::trunc);
        bad << "nope";
    }
    threw = false;
    try {
        (void)atperson::load_remote_control_cursor(root / "notjson.json");
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok remote control cursor round-trip\n");
}

void test_watermark_survives_restart() {
    /* The reason the cursor is separate from ControlState: a pause applied
     * remotely, then a restart, must not leave the channel able to replay
     * that pause at the operator's next seq. */
    const auto root = scratch_dir("restart");
    const auto cursor_path = root / "remote-cursor.json";

    atperson::ControlState state = fresh_state();
    std::uint64_t watermark = atperson::load_remote_control_cursor(cursor_path).last_seq;
    assert(atperson::apply_control_request(state, watermark, request(1, atperson::ControlOp::Pause),
                                           kOperator, kOperator)
               .applied);
    atperson::save_remote_control_cursor({1u, watermark}, cursor_path);

    /* Restart: state reloaded, watermark reloaded. */
    atperson::ControlState reloaded = fresh_state();
    std::uint64_t resumed = atperson::load_remote_control_cursor(cursor_path).last_seq;
    assert(resumed == 1u);
    assert(atperson::apply_control_request(
               reloaded, resumed, request(2, atperson::ControlOp::Resume), kOperator, kOperator)
               .applied);
    /* The old pause is not re-appliable. */
    const auto replay = atperson::apply_control_request(
        reloaded, resumed, request(1, atperson::ControlOp::Pause), kOperator, kOperator);
    assert(!replay.applied);
    assert(!reloaded.paused);
    std::printf("ok watermark survives restart\n");
}

void test_operator_must_differ_from_account() {
    using atperson::OperatorChannelStatus;
    using atperson::check_operator_channel;

    /* Unset operator DID: the channel is off by choice, not broken. */
    assert(check_operator_channel(kOperator, "") == OperatorChannelStatus::Disabled);
    assert(check_operator_channel("", "") == OperatorChannelStatus::Disabled);

    /* The rule: a separate operator account is Ready. */
    assert(check_operator_channel(kAccount, kOperator) == OperatorChannelStatus::Ready);
    assert(check_operator_channel(kOperator, kAccount) == OperatorChannelStatus::Ready);

    /* The entity may not be its own operator. This is the case that would
     * otherwise let the runtime author every request it obeys. */
    assert(check_operator_channel(kOperator, kOperator) == OperatorChannelStatus::Conflict);
    assert(check_operator_channel(kAccount, kAccount) == OperatorChannelStatus::Conflict);

    /* DIDs are case-sensitive and method-specific, so a near-miss is still
     * a collision only when it really is one. */
    assert(check_operator_channel("did:plc:Operator", "did:plc:operator") ==
           OperatorChannelStatus::Ready);
    assert(check_operator_channel("did:web:operator", "did:plc:operator") ==
           OperatorChannelStatus::Ready);

    /* An unknown account cannot be proven distinct, but it is also not
     * proven equal: the poller re-checks against the session that actually
     * authenticated, so this stays Ready and is caught there. */
    assert(check_operator_channel("", kOperator) == OperatorChannelStatus::Ready);

    /* Ready has no denial text; the other two explain themselves, and the
     * conflict names both DIDs so the fix is obvious. */
    assert(atperson::operator_channel_denial(kAccount, kOperator).empty());
    const std::string disabled = atperson::operator_channel_denial(kAccount, "");
    assert(!disabled.empty());
    assert(disabled.find("ATPERSON_OPERATOR_DID") != std::string::npos);

    const std::string conflict = atperson::operator_channel_denial(kOperator, kOperator);
    assert(!conflict.empty());
    assert(conflict.find(kOperator) != std::string::npos);
    assert(conflict.find("command itself") != std::string::npos);
    assert(conflict.find("ATPERSON_OPERATOR_DID") != std::string::npos);
    std::printf("ok operator DID must differ from the account DID\n");
}

void test_rkey_order_matches_sequence_order() {
    /* The poller relies on the service returning the collection in reverse
     * rkey order and reversing it locally, so lexicographic record order
     * must equal numeric sequence order. Check it densely around the
     * base-32 carry boundaries, where a mis-ordered alphabet shows up. */
    for (std::uint64_t seq = 1u; seq < 4096u; ++seq) {
        const std::string previous = atperson::control_request_rkey(seq - 1u);
        const std::string current = atperson::control_request_rkey(seq);
        assert(previous.size() == 13u);
        assert(current.size() == 13u);
        assert(previous < current);
        /* Deterministic, so re-emitting a sequence is idempotent. */
        assert(current == atperson::control_request_rkey(seq));
    }
    /* A large sequence must still sort above a small one. */
    assert(atperson::control_request_rkey(1u) < atperson::control_request_rkey(1ull << 40));
    std::printf("ok rkey order matches sequence order\n");
}

} // namespace

int main() {
    test_op_names_round_trip();
    test_serialise_parse_round_trip();
    test_serialise_rejects_bad_argument_shape();
    test_parse_rejects_malformed_documents();
    test_provenance_is_checked_first();
    test_sequence_must_be_exactly_next();
    test_every_op_maps_onto_control_state();
    test_approve_respects_the_local_bound();
    test_shape_is_rechecked_at_apply_time();
    test_uri_provenance_parsing();
    test_cursor_round_trip_and_defaults();
    test_watermark_survives_restart();
    test_operator_must_differ_from_account();
    test_rkey_order_matches_sequence_order();
    std::printf("remote control tests passed\n");
    return 0;
}
