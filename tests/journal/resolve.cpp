/* Pre-action expectation resolution tests (#149): the pure derivation
 * semantics (met / unmet / expired / pending / none), the window boundaries
 * (a reply exactly on the boundary is on time; expiry is strictly beyond),
 * and the idempotent resolution pass (pending stays unwritten, reruns
 * append nothing, a late reference after expiry records the knowledge change).
 * Offline, no network. */

#include "journal/resolve.hpp"
#include "journal/store.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using atperson::JournalAction;
using atperson::JournalActionOutcome;
using atperson::JournalContents;
using atperson::JournalEvent;
using atperson::JournalExpectation;
using atperson::JournalExpectationState;
using atperson::ResolutionReport;
using atperson::derive_expectation_state;

using atperson::kExpectationWindowSeconds;

/* "2026-09-17T10:00:00Z" = 1789639200; the expectation window (7 days)
 * closes at 1790244000. */
constexpr std::int64_t kActionEpoch = 1789639200;
constexpr std::int64_t kCutoff = kActionEpoch + 604800;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-journal-resolve-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

JournalAction executed_with_expectation(const std::string &id, const std::string &at) {
    JournalAction action;
    action.id = id;
    action.kind = "post";
    action.text = "the moon is a loyal companion";
    action.digest = "0123456789abcdef";
    action.outcome = JournalActionOutcome::Executed;
    action.reason = "allow";
    action.uri = "at://did:plc:example/app.bsky.feed.post/" + id;
    action.cid = "bafyreiabc123";
    action.at = at;
    atperson::JournalExpectation expectation;
    expectation.kind = "approach";
    expectation.reply_likelihood = 0.7f;
    expectation.tokens = {"moon"};
    action.expectation = expectation;
    return action;
}

JournalEvent linked_event(const std::string &action_id, const std::string &at) {
    JournalEvent event;
    event.action_id = action_id;
    event.event_uri = "at://did:plc:other/app.bsky.feed.post/reply-" + action_id;
    event.author_did = "did:plc:other";
    event.via = "parent";
    event.at = at;
    return event;
}

std::size_t resolution_count(const std::filesystem::path &journal_path) {
    return atperson::load_journal(journal_path).resolutions.size();
}

void test_derive_states() {
    const JournalAction action = executed_with_expectation("act", "2026-09-17T10:00:00Z");
    const std::vector<JournalEvent> none;

    /* No events yet: pending while the window is open, expired strictly
     * beyond it. */
    assert(derive_expectation_state(action, none, kActionEpoch) ==
           JournalExpectationState::Pending);
    assert(derive_expectation_state(action, none, kCutoff) == JournalExpectationState::Pending);
    assert(derive_expectation_state(action, none, kCutoff + 1) ==
           JournalExpectationState::Expired);

    /* A reply exactly on the boundary counts as on time (met). */
    const std::vector<JournalEvent> on_time{linked_event("act", "2026-09-24T10:00:00Z")};
    assert(derive_expectation_state(action, on_time, kCutoff + 1) ==
           JournalExpectationState::Met);

    /* A reply inside the window is met regardless of when we judge. */
    const std::vector<JournalEvent> early{linked_event("act", "2026-09-17T12:00:00Z")};
    assert(derive_expectation_state(action, early, kActionEpoch + 1) ==
           JournalExpectationState::Met);

    /* A reply exactly at the attempt instant is contact that did not land in
     * time: unmet, never met. */
    const std::vector<JournalEvent> same_instant{linked_event("act", "2026-09-17T10:00:00Z")};
    assert(derive_expectation_state(action, same_instant, kCutoff + 1) ==
           JournalExpectationState::Unmet);

    /* A late reply (after the window) is unmet: contact happened, but not in
     * time. */
    const std::vector<JournalEvent> late{linked_event("act", "2026-09-25T10:00:00Z")};
    assert(derive_expectation_state(action, late, kCutoff + 1) ==
           JournalExpectationState::Unmet);

    /* An event whose instant cannot be parsed counts as contact that never
     * landed in time: unmet, consistent with the unparseable-instant
     * convention elsewhere. */
    const std::vector<JournalEvent> anonymous{linked_event("act", "not-a-time")};
    assert(derive_expectation_state(action, anonymous, kCutoff + 1) ==
           JournalExpectationState::Unmet);

    /* Only executed, self-decided actions resolve. */
    JournalAction denied = executed_with_expectation("denied", "2026-09-17T10:00:00Z");
    denied.outcome = JournalActionOutcome::Denied;
    denied.uri.clear();
    assert(derive_expectation_state(denied, early, kCutoff + 1) == JournalExpectationState::None);

    JournalAction unauthored = executed_with_expectation("manual", "2026-09-17T10:00:00Z");
    unauthored.expectation = std::nullopt;
    assert(derive_expectation_state(unauthored, early, kCutoff + 1) ==
           JournalExpectationState::None);

    /* An attempt without a parseable instant has no window to judge. */
    JournalAction untimed = executed_with_expectation("untimed", "not-a-time");
    assert(derive_expectation_state(untimed, late, kCutoff + 1) == JournalExpectationState::None);

    std::printf("ok derive states\n");
    (void)none;
}

void test_resolution_pass_idempotent_and_late_change() {
    const auto root = scratch_dir("pass");
    const auto journal_path = root / "action-journal.jsonl";

    atperson::append_journal_action(journal_path,
                                    executed_with_expectation("act-A", "2026-09-17T10:00:00Z"));
    atperson::append_journal_action(journal_path,
                                    executed_with_expectation("act-B", "2026-09-17T10:00:00Z"));
    atperson::append_journal_action(journal_path,
                                    executed_with_expectation("act-C", "2026-09-17T10:00:00Z"));
    atperson::append_journal_action(journal_path,
                                    executed_with_expectation("act-D", "2026-09-17T10:00:00Z"));
    /* act-B and act-D drew an in-window reply; act-A and act-C did not. */
    atperson::append_journal_event(journal_path, linked_event("act-B", "2026-09-17T12:00:00Z"));
    atperson::append_journal_event(journal_path, linked_event("act-D", "2026-09-17T12:00:00Z"));

    /* Pass 1 (window open): act-B and act-D are met and recorded; act-A and
     * act-C are still pending and stay unwritten. */
    const ResolutionReport pass1 = atperson::resolve_expectations(
        journal_path, kActionEpoch + 1, "2026-09-17T10:00:01Z");
    assert(pass1.evaluated == 4u);
    assert(pass1.pending == 2u);
    assert(pass1.met_written == 2u);
    assert(pass1.unmet_written == 0u);
    assert(pass1.expired_written == 0u);
    assert(pass1.state_changed == 0u);
    assert(pass1.skipped_already == 0u);
    assert(resolution_count(journal_path) == 2u);

    /* Pass 2 at the exact boundary (window still open, nothing new):
     * terminal states are already recorded, pending states stay unwritten. */
    const ResolutionReport pass2 = atperson::resolve_expectations(
        journal_path, kCutoff, "2026-09-24T10:00:00Z");
    assert(pass2.evaluated == 4u);
    assert(pass2.pending == 2u);
    assert(pass2.skipped_already == 2u);
    assert(pass2.met_written == 0u);
    assert(resolution_count(journal_path) == 2u);

    /* Later, act-A finally draws a reply after its window closed, while
     * act-C's window closes untouched. At judgement time (~2026-09-25):
     *  - act-A: late contact => unmet (first recorded state, no change);
     *  - act-B: met remains met (its in-window reply persists);
     *  - act-C: no contact, window closed => expired;
     *  - act-D: met remains met for the same reason. */
    atperson::append_journal_event(journal_path, linked_event("act-A", "2026-09-25T10:00:00Z"));
    const ResolutionReport pass3 = atperson::resolve_expectations(
        journal_path, kCutoff + 86400, "2026-09-25T10:00:00Z");
    assert(pass3.evaluated == 4u);
    assert(pass3.pending == 0u);
    assert(pass3.met_written == 0u);
    assert(pass3.unmet_written == 1u);
    assert(pass3.expired_written == 1u);
    assert(pass3.state_changed == 0u);
    assert(pass3.skipped_already == 2u);

    /* A late reference discovered after expiry is a real knowledge change:
     * act-C was recorded expired, then contact surfaces late, so a second
     * resolution line records the expired -> unmet flip. */
    atperson::append_journal_event(journal_path, linked_event("act-C", "2026-09-26T09:00:00Z"));
    const ResolutionReport pass4 = atperson::resolve_expectations(
        journal_path, kCutoff + 172800, "2026-09-26T10:00:00Z");
    assert(pass4.evaluated == 4u);
    assert(pass4.pending == 0u);
    assert(pass4.met_written == 0u);
    assert(pass4.unmet_written == 1u);
    assert(pass4.expired_written == 0u);
    assert(pass4.state_changed == 1u);
    assert(pass4.skipped_already == 3u);

    const JournalContents journal = atperson::load_journal(journal_path);
    assert(journal.resolutions.size() == 5u);
    std::size_t met = 0u, unmet = 0u, expired = 0u;
    std::size_t act_c_lines = 0u;
    for (const auto &entry : journal.resolutions) {
        if (entry.action_id == "act-C") {
            ++act_c_lines;
        }
        switch (entry.state) {
        case JournalExpectationState::Met:
            ++met;
            break;
        case JournalExpectationState::Unmet:
            ++unmet;
            break;
        case JournalExpectationState::Expired:
            ++expired;
            break;
        default:
            break;
        }
    }
    assert(met == 2u);
    assert(unmet == 2u);
    assert(expired == 1u);
    assert(act_c_lines == 2u);
    assert(journal.resolutions[0].at_epoch == static_cast<std::uint64_t>(kActionEpoch + 1));
    assert(journal.resolutions[3].action_id == "act-C");
    assert(journal.resolutions[3].state == JournalExpectationState::Expired);
    assert(journal.resolutions[4].action_id == "act-C");
    assert(journal.resolutions[4].state == JournalExpectationState::Unmet);

    /* Pass 5 (idempotent): everything is terminal and recorded; nothing
     * changes again. */
    const ResolutionReport pass5 = atperson::resolve_expectations(
        journal_path, kCutoff + 172801, "2026-09-26T10:00:01Z");
    assert(pass5.skipped_already == 4u);
    assert(pass5.met_written == 0u && pass5.unmet_written == 0u && pass5.expired_written == 0u);
    assert(resolution_count(journal_path) == 5u);

    std::printf("ok resolution pass idempotent and late change\n");
}

void test_resolve_skips_foreign_entries() {
    const auto root = scratch_dir("foreign");
    const auto journal_path = root / "action-journal.jsonl";

    /* A denied action with a recorded prediction is not an executed decision;
     * an executed action without a prediction is operator-authored. Neither
     * is an expectation to resolve. */
    JournalAction denied = executed_with_expectation("denied", "2026-09-17T10:00:00Z");
    denied.outcome = JournalActionOutcome::Denied;
    denied.uri.clear();
    atperson::append_journal_action(journal_path, denied);

    JournalAction manual = executed_with_expectation("manual", "2026-09-17T10:00:00Z");
    manual.expectation = std::nullopt;
    atperson::append_journal_action(journal_path, manual);

    const ResolutionReport report = atperson::resolve_expectations(
        journal_path, kCutoff + 1, "2026-09-25T10:00:01Z");
    assert(report.evaluated == 0u);
    assert(report.pending == 0u);
    assert(report.met_written == 0u && report.unmet_written == 0u && report.expired_written == 0u);
    assert(resolution_count(journal_path) == 0u);

    std::printf("ok resolve skips foreign entries\n");
}

} // namespace

int main() {
    test_derive_states();
    test_resolution_pass_idempotent_and_late_change();
    test_resolve_skips_foreign_entries();
    std::printf("atperson-journal-resolve: all tests passed\n");
    return 0;
}