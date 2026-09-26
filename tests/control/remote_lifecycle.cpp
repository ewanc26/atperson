/* Issue #143 remote operator control, exercised through the durable
 * lifecycle.
 *
 * Part of the deterministic end-to-end lifecycle harness; see
 * tests/support/lifecycle_harness.hpp for the shared fixtures. Offline and
 * deterministic.
 *
 * The acceptance criterion for persistent hosting is that an operator can
 * pause, resume and approve remotely, and that this is *demonstrated*, not
 * only unit-tested. The demonstration has to show the commands matter to a
 * running host, not just that a switch flipped in memory:
 *
 *   - a remote pause actually stops the next cycle's ingestion, on a host
 *     that has to read the pause back from disk to find it;
 *   - the watermark is durable, so a restart cannot be used to replay an
 *     old pause over a later resume;
 *   - a remote approve binds one exact digest, and the gate admits that
 *     action and no other;
 *   - the separation rule holds on the real path a host would take: a
 *     channel whose operator is the account is not a channel.
 *
 * The record *fetch* is the one part that needs a PDS, so it is out of
 * scope here and covered by the poll tests; what this file drives is the
 * part that touches durable state, over real files. */

#include "support/lifecycle_harness.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "control/remote.hpp"
#include "control/state.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

namespace atperson::e2e {
namespace {

constexpr std::string_view kAccount = "did:plc:entity";
constexpr std::string_view kOperator = "did:plc:operator";
constexpr std::string_view kControlUri = "at://did:plc:operator/click.croft.atperson.control/x";

/* A command as the operator published it. */
ControlRequest request(std::uint64_t seq, ControlOp op, std::string arg = {}) {
    ControlRequest published;
    published.seq = seq;
    published.op = op;
    published.arg = std::move(arg);
    published.at = "2026-09-26T12:00:00Z";
    return published;
}

/* One daemon cycle's remote-control pass, minus the fetch: drop records the
 * poller would drop, apply what it would apply, save the state, advance and
 * save the watermark. Returns whether the record was applied.
 *
 * The authority comes from the at-URI the service returned, never from the
 * document, and the save order is state-then-watermark — the two properties
 * the durable replay guarantee rests on. */
struct Channel {
    std::filesystem::path control_file;
    std::filesystem::path cursor_file;

    bool apply(const ControlRequest &published, std::string_view uri = kControlUri) {
        const std::optional<std::string> authority = aturi_authority(uri);
        if (!authority.has_value() || *authority != kOperator) {
            /* Another account's record: not a command, never even parsed,
             * and the watermark unmoved. */
            return false;
        }

        RemoteControlCursor cursor = load_remote_control_cursor(cursor_file);
        ControlState state = load_control_state(control_file);
        const RemoteApplyReport report =
            apply_control_request(state, cursor.last_seq, published, kOperator, *authority);
        if (!report.applied) {
            return false;
        }
        save_control_state(state, control_file);
        cursor.last_seq = published.seq;
        save_remote_control_cursor(cursor, cursor_file);
        return true;
    }
};

/* Whether the production outbound choke point refuses this digest. The gate
 * throws rather than returning a bool, so the assertion reads cleanly as
 * "refused" and the real check chain — not paused, writes enabled, not
 * dry-run, approved — is what is under test. */
bool outbound_refused(const ControlState &state, std::string_view digest) {
    try {
        ensure_outbound_allowed(state, digest);
        return false;
    } catch (const ControlStateError &) {
        return true;
    }
}

void test_remote_pause_survives_restart_and_blocks_ingestion() {
    Scenario scenario("remote-pause");
    Channel channel{scenario.control_file(), scenario.dir / "remote-control-cursor.json"};
    ScriptedFeed feed({
        {{obs("at://e2e/r1", "alpha beta")}, std::nullopt},
    });

    /* The host learns normally before the operator says anything. */
    const auto first = scenario.run(feed, 1);
    assert(first.observations_seen == 1u);
    assert(first.learned == 1u);

    /* Remote pause, applied by the operator's record. */
    assert(channel.apply(request(1u, ControlOp::Pause)));
    assert(load_control_state(scenario.control_file()).paused);

    /* The next cycle reads the pause from disk on a cold open and does no
     * work at all — this is the property a headless host depends on. */
    const auto paused = scenario.run_cycle(feed, 1);
    assert(paused.paused);
    assert(paused.result.observations_seen == 0u);
    assert(paused.result.learned == 0u);

    /* Nothing was learned while paused, and the ledger did not grow. */
    Ledger ledger(scenario.ledger_file);
    assert(ledger.count() == 1u);

    /* Remote resume; the host learns again. */
    assert(channel.apply(request(2u, ControlOp::Resume)));
    assert(!load_control_state(scenario.control_file()).paused);
    const auto resumed = scenario.run_cycle(feed, 1);
    assert(!resumed.paused);
    assert(resumed.result.observations_seen == 1u);
    /* Re-seeing the same source is a duplicate, not a second lesson. */
    assert(resumed.result.duplicates == 1u);
    assert(resumed.result.learned == 0u);

    std::puts("remote pause blocks ingestion and survives a restart: ok");
}

void test_remote_watermark_survives_restart_and_refuses_replay() {
    /* The reason the watermark is its own file: a host that restarts must
     * not become a replay oracle. Pausing again with an old sequence
     * number is refused after a resume, so a stale record sitting in the
     * operator's repo cannot re-pause the entity. */
    Scenario scenario("remote-watermark");
    Channel channel{scenario.control_file(), scenario.dir / "remote-control-cursor.json"};

    assert(channel.apply(request(1u, ControlOp::Pause)));
    assert(channel.apply(request(2u, ControlOp::Resume)));
    assert(load_control_state(scenario.control_file()).paused == false);

    /* A fresh channel object: a restart. The watermark is read from disk,
     * not from whatever this process happened to be holding. */
    Channel after_restart{scenario.control_file(), scenario.dir / "remote-control-cursor.json"};
    assert(load_remote_control_cursor(after_restart.cursor_file).last_seq == 2u);

    /* Replay of the old pause: refused. */
    assert(!after_restart.apply(request(1u, ControlOp::Pause)));
    assert(load_control_state(scenario.control_file()).paused == false);

    /* A gap: refused, and the watermark does not move, so the missing
     * command can still arrive. */
    assert(!after_restart.apply(request(9u, ControlOp::Pause)));
    assert(load_remote_control_cursor(after_restart.cursor_file).last_seq == 2u);
    assert(load_control_state(scenario.control_file()).paused == false);

    /* The corrected re-send at the next admissible sequence works, and a
     * refused record having changed nothing is what makes that possible. */
    assert(after_restart.apply(request(3u, ControlOp::Pause)));
    assert(load_control_state(scenario.control_file()).paused);

    std::puts("remote watermark survives restart and refuses replay: ok");
}

void test_remote_approve_binds_one_exact_action() {
    /* Approval is a digest of an inspected decision, so the remote approve
     * must bind the same digest the outbound choke point checks — and only
     * that digest. A regenerated plan produces a different digest and is not
     * covered, which is the property that makes approving remotely safe. The
     * gate exercised here is the real one (`ensure_outbound_allowed`), not a
     * restatement of it. */
    Scenario scenario("remote-approve");
    Channel channel{scenario.control_file(), scenario.dir / "remote-control-cursor.json"};

    /* Writes must be on for an approval to mean anything, and the operator
     * does that over the channel too. */
    assert(channel.apply(request(1u, ControlOp::WritesOn)));
    assert(channel.apply(request(2u, ControlOp::DryRunOff)));
    ControlState state = load_control_state(scenario.control_file());
    assert(state.writes_enabled);
    assert(!state.dry_run);
    assert(state.approval_required);

    /* Fail-closed from here: the gate refuses until an exact digest is
     * approved. */
    const std::string digest = "0123456789abcdef";
    assert(outbound_refused(state, digest));

    assert(channel.apply(request(3u, ControlOp::Approve, digest)));
    state = load_control_state(scenario.control_file());
    assert(state.approved_digests.size() == 1u);
    assert(state.approved_digests.front() == digest);

    /* The gate admits exactly that digest: the approved action, and not a
     * neighbouring one, not the empty digest, not a re-planned action. */
    assert(!outbound_refused(state, digest));
    assert(outbound_refused(state, "fedcba9876543210"));
    assert(outbound_refused(state, ""));

    /* Remote revoke withdraws it again, and the gate closes. */
    assert(channel.apply(request(4u, ControlOp::Revoke, digest)));
    state = load_control_state(scenario.control_file());
    assert(state.approved_digests.empty());
    assert(outbound_refused(state, digest));

    /* Turning approval off opens the approval gate, and nothing else: the
     * operator asked for it over the same channel, and policy and budget
     * downstream are untouched by a control switch. */
    assert(channel.apply(request(5u, ControlOp::ApprovalOff)));
    state = load_control_state(scenario.control_file());
    assert(!state.approval_required);
    assert(state.approved_digests.empty());
    assert(!outbound_refused(state, digest));

    /* Pause outranks everything above: a remote pause closes the gate even
     * with writes on, dry-run off and approval waived. */
    assert(channel.apply(request(6u, ControlOp::Pause)));
    state = load_control_state(scenario.control_file());
    assert(state.paused);
    assert(outbound_refused(state, digest));

    std::puts("remote approve binds one exact action: ok");
}

void test_foreign_record_is_not_a_command() {
    /* A record published by anyone other than the operator is data, not a
     * command. Provenance is read from the at-URI the service returned,
     * never from the document, so a record cannot claim to be the
     * operator's. */
    Scenario scenario("remote-foreign");
    Channel channel{scenario.control_file(), scenario.dir / "remote-control-cursor.json"};

    const ControlRequest foreign = request(1u, ControlOp::Pause);
    assert(!channel.apply(foreign, "at://did:plc:stranger/click.croft.atperson.control/x"));
    assert(!load_control_state(scenario.control_file()).paused);
    /* And the refusal consumed no sequence, so the operator's own seq 1 is
     * still admissible afterwards. */
    assert(load_remote_control_cursor(channel.cursor_file).last_seq == 0u);
    assert(channel.apply(request(1u, ControlOp::Pause)));
    assert(load_control_state(scenario.control_file()).paused);

    std::puts("a record from another account is not a command: ok");
}

void test_operator_channel_is_refused_when_it_is_the_account() {
    /* The separation rule, on the real path a host would take. A channel
     * whose operator is the account is not a channel: the entity could
     * command itself. */
    assert(check_operator_channel(kAccount, kOperator) == OperatorChannelStatus::Ready);
    assert(check_operator_channel(kOperator, kOperator) == OperatorChannelStatus::Conflict);
    assert(check_operator_channel(kAccount, "") == OperatorChannelStatus::Disabled);
    const std::string denial = operator_channel_denial(kOperator, kOperator);
    assert(denial.find("command itself") != std::string::npos);

    std::puts("operator channel refused when it is the account: ok");
}

} // namespace

void run_remote_control_scenarios() {
    test_remote_pause_survives_restart_and_blocks_ingestion();
    test_remote_watermark_survives_restart_and_refuses_replay();
    test_remote_approve_binds_one_exact_action();
    test_foreign_record_is_not_a_command();
    test_operator_channel_is_refused_when_it_is_the_account();
}

} // namespace atperson::e2e
