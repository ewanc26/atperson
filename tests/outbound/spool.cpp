/* Offline-safe network writes (#154): spool-first writes, offline mode,
 * drain-on-restore, denied permanence, transport-failure suffix retention
 * and inspection. Offline: fake OutboundWriter, real gates, real fs. */
#include "outbound/spool.hpp"
#include "outbound/attempt.hpp"
#include "state/records.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"
#include "outbound/action.hpp"
#include "outbound/audit.hpp"
#include "outbound/budget.hpp"
#include "outbound/config.hpp"
#include "outbound/evaluate.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using atperson::ControlState;
using atperson::OutboundAction;
using atperson::OutboundActionKind;
using atperson::OutboundAttemptPaths;
using atperson::OutboundExecutionOutcome;
using atperson::OutboundPolicy;
using atperson::OutboundWriter;
using atperson::OutboundWriteResult;
using atperson::SpoolDrainReport;
using atperson::SpoolPaths;
using atperson::SpoolStatus;

constexpr std::int64_t NOW = 1'700'000'000;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-spool-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

OutboundAction post_action(const char *rkey, const char *text) {
    OutboundAction action;
    action.kind = OutboundActionKind::Post;
    action.text = text;
    action.rkey = rkey;
    action.created_at = "2026-09-17T00:00:00Z";
    action.digest = "0123456789abcdef";
    return action;
}

struct FakeWriter final : OutboundWriter {
    int put_calls = 0;
    bool fail_put = false;
    std::string fail_put_on;
    std::vector<std::string> rkeys;

    std::string resolve_record_cid(const std::string &) override { return "bafycid"; }

    OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                   const std::string &) override {
        ++put_calls;
        rkeys.push_back(rkey);
        if (fail_put || rkey == fail_put_on) {
            throw std::runtime_error("transport failed");
        }
        OutboundWriteResult written;
        written.uri = "at://did:plc:self/" + collection + "/" + rkey;
        written.cid = "bafyrecord";
        return written;
    }
};

/* Gate files that allow everything: the spool's own semantics are under
 * test, not the gate chain (covered by the publish tests). */
struct Gates {
    std::filesystem::path policy;
    std::filesystem::path budget;
    std::filesystem::path control;
    std::filesystem::path audit;
    std::filesystem::path journal;
    std::filesystem::path envelopes;
    std::filesystem::path spool_root;

    explicit Gates(const std::filesystem::path &root)
        : policy(root / "policy.json"), budget(root / "budget.json"),
          control(root / "control.json"), audit(root / "audit.jsonl"),
          journal(root / "journal.jsonl"), envelopes(root / "envelopes"),
          spool_root(root / "offline-spool") {}

    void save(const ControlState &state) const {
        atperson::save_control_state(state, control);
    }

    [[nodiscard]] ControlState open_control() const {
        ControlState state;
        state.paused = false;
        state.writes_enabled = true;
        state.dry_run = false;
        state.approval_required = false;
        return state;
    }

    [[nodiscard]] OutboundAttemptPaths attempt_paths() const {
        return {policy, budget, control, audit, journal, envelopes, spool_root};
    }
};

void save_allow_all_policy(const std::filesystem::path &path) {
    OutboundPolicy policy;
    atperson::ActionBudget budget;
    budget.enabled = true;
    budget.max_in_window = 100;
    budget.window_seconds = 3600;
    budget.min_interval_seconds = 0;
    budget.duplicate_cooldown_seconds = 0;
    atperson::budget_for(policy, OutboundActionKind::Post) = budget;
    atperson::write_record(path.parent_path(), path.stem().string(),
                              atperson::serialise_outbound_policy(policy));
}

void test_spool_first_write() {
    const std::filesystem::path root = scratch_dir("first");
    Gates gates(root);
    gates.save(gates.open_control());
    save_allow_all_policy(gates.policy);

    FakeWriter writer;
    writer.fail_put = true;
    const auto writer_for = [&writer]() -> OutboundWriter & { return writer; };

    /* A transport failure leaves the record spooled: implicit offline. */
    const OutboundAction action = post_action("3kabc", "hello network");
    const auto result = atperson::attempt_outbound_action(action, gates.attempt_paths(),
                                                          writer_for, NOW);
    assert(result.outcome == OutboundExecutionOutcome::Failed);

    const SpoolPaths spool{gates.spool_root};
    SpoolStatus status = atperson::spool_status(spool);
    assert(status.pending_count == 1u);
    assert(status.earliest_pending_at.has_value());

    /* Recovery: the drain publishes the spooled record through the same
     * gates, idempotent on the frozen rkey. */
    writer.fail_put = false;
    const SpoolDrainReport drained =
        atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(drained.published == 1u);
    /* One failed live attempt + one drain replay, same frozen rkey. */
    assert(writer.rkeys.size() == 2u);
    assert(writer.rkeys[0] == "3kabc");
    assert(writer.rkeys[1] == "3kabc");

    status = atperson::spool_status(spool);
    assert(status.pending_count == 0u);
    std::puts("spool-first write + drain: ok");
}

void test_offline_mode_spools_without_network() {
    const std::filesystem::path root = scratch_dir("offline");
    Gates gates(root);
    ControlState control = gates.open_control();
    control.offline_mode = true;
    gates.save(control);
    save_allow_all_policy(gates.policy);

    FakeWriter writer;
    const auto writer_for = [&writer]() -> OutboundWriter & { return writer; };

    /* Offline mode: N attempts create N spool entries and zero network
     * sessions. */
    const OutboundAction first = post_action("3ka", "first");
    const OutboundAction second = post_action("3kb", "second");
    const auto r1 = atperson::attempt_outbound_action(first, gates.attempt_paths(),
                                                       writer_for, NOW);
    const auto r2 = atperson::attempt_outbound_action(second, gates.attempt_paths(),
                                                       writer_for, NOW);
    assert(r1.outcome != OutboundExecutionOutcome::Executed);
    assert(r2.outcome != OutboundExecutionOutcome::Executed);
    assert(writer.put_calls == 0);

    const SpoolPaths spool{gates.spool_root};
    assert(atperson::spool_status(spool).pending_count == 2u);

    /* Restoring online mode drains automatically, in creation order,
     * through all gates, with no duplicates. */
    control.offline_mode = false;
    gates.save(control);
    const SpoolDrainReport drained =
        atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(drained.published == 2u);
    assert(writer.put_calls == 2);
    assert(writer.rkeys[0] == "3ka");
    assert(writer.rkeys[1] == "3kb");
    assert(atperson::spool_status(spool).pending_count == 0u);
    std::puts("offline mode spools, restore drains: ok");
}

void test_denied_entry_is_permanent() {
    const std::filesystem::path root = scratch_dir("denied");
    Gates gates(root);
    gates.save(gates.open_control());
    save_allow_all_policy(gates.policy);

    FakeWriter writer;
    writer.fail_put = true;
    const auto writer_for = [&writer]() -> OutboundWriter & { return writer; };

    const OutboundAction action = post_action("3kdenied", "will be refused at drain");
    (void)atperson::attempt_outbound_action(action, gates.attempt_paths(), writer_for, NOW);
    const SpoolPaths spool{gates.spool_root};
    assert(atperson::spool_status(spool).pending_count == 1u);
    /* One failed live attempt reached the writer before failing. */
    const int calls_before_drain = writer.put_calls;
    assert(calls_before_drain == 1);

    /* The operator pauses: the drain refuses, and the refusal is
     * permanent — the entry moves to denied/ and never retries. The
     * pause gate refuses before any writer call. */
    ControlState paused = gates.open_control();
    paused.paused = true;
    gates.save(paused);
    writer.fail_put = false;
    const SpoolDrainReport first_drain =
        atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(first_drain.denied == 1u);
    assert(writer.put_calls == calls_before_drain);

    const SpoolStatus status = atperson::spool_status(spool);
    assert(status.pending_count == 0u);
    assert(status.denied_count == 1u);

    /* Unpausing does not resurrect a denied entry. */
    gates.save(gates.open_control());
    const SpoolDrainReport second_drain =
        atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(second_drain.published == 0u);
    assert(second_drain.denied == 0u);
    std::puts("denied entry is permanent: ok");
}

void test_transport_failure_retains_suffix() {
    const std::filesystem::path root = scratch_dir("suffix");
    Gates gates(root);
    gates.save(gates.open_control());
    save_allow_all_policy(gates.policy);

    FakeWriter writer;
    writer.fail_put = true;
    const auto writer_for = [&writer]() -> OutboundWriter & { return writer; };

    /* Three records spool on transport failure. */
    for (const char *rkey : {"3k1", "3k2", "3k3"}) {
        (void)atperson::attempt_outbound_action(post_action(rkey, "suffix test"),
                                                 gates.attempt_paths(), writer_for, NOW);
    }
    const SpoolPaths spool{gates.spool_root};
    assert(atperson::spool_status(spool).pending_count == 3u);

    /* The drain fails mid-way on 3k2: only the uncommitted suffix stays
     * spooled, and the next drain resumes there. */
    writer.fail_put = false;
    writer.fail_put_on = "3k2";
    SpoolDrainReport drained =
        atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(drained.published == 1u);
    assert(drained.failed == 1u);
    assert(drained.transport_failed);
    assert(atperson::spool_status(spool).pending_count == 2u);

    writer.fail_put_on.clear();
    drained = atperson::spool_drain(spool, gates.attempt_paths(), writer_for, NOW);
    assert(drained.published == 2u);
    assert(atperson::spool_status(spool).pending_count == 0u);
    std::puts("transport failure retains suffix, drain resumes: ok");
}

void test_corrupt_entry_is_reported() {
    const std::filesystem::path root = scratch_dir("corrupt");
    Gates gates(root);
    gates.save(gates.open_control());
    save_allow_all_policy(gates.policy);

    const SpoolPaths spool{gates.spool_root};
    std::filesystem::create_directories(spool.pending());
    std::ofstream(spool.pending() / "00000000000000000001.json")
        << "{\"format\":\"nope\"}";

    bool threw = false;
    try {
        (void)atperson::spool_pending(spool);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    std::puts("corrupt entry is reported, not reinterpreted: ok");
}

} // namespace

int main() {
    test_spool_first_write();
    test_offline_mode_spools_without_network();
    test_denied_entry_is_permanent();
    test_transport_failure_retains_suffix();
    test_corrupt_entry_is_reported();
    return 0;
}
