#include "resolve.hpp"

#include "state/time.hpp"

#include <cstdint>
#include <string>

namespace atperson {

JournalExpectationState derive_expectation_state(const JournalAction &action,
                                                 const std::vector<JournalEvent> &events,
                                                 std::int64_t now) {
    /* Only executed autonomous actions carry a resolvable prediction. */
    if (!action.expectation.has_value() ||
        action.outcome != JournalActionOutcome::Executed) {
        return JournalExpectationState::None;
    }
    /* An attempt instant that does not parse is unknown time (the same
     * convention rules.cpp and the sync engine use); without it there is
     * no window to judge, so no resolution is derived. */
    const std::uint64_t action_epoch = parse_rfc3339_epoch(action.at).value_or(0u);
    if (action_epoch == 0u) {
        return JournalExpectationState::None;
    }
    bool any_contact = false;
    const std::uint64_t window = static_cast<std::uint64_t>(kExpectationWindowSeconds);
    for (const JournalEvent &event : events) {
        if (event.action_id != action.id) {
            continue;
        }
        any_contact = true;
        const std::uint64_t event_epoch = parse_rfc3339_epoch(event.at).value_or(0u);
        if (event_epoch > action_epoch && event_epoch - action_epoch <= window) {
            return JournalExpectationState::Met;
        }
    }
    if (any_contact) {
        /* Contact happened, but nothing landed inside the window: the
         * reference was late, so the prediction was unmet. An event with an
         * unparseable instant is contact that never counted as on time. */
        return JournalExpectationState::Unmet;
    }
    /* No contact yet: pending until the window closes, then expired. At
     * exactly attempt + window the window is still open; expiry is strictly
     * beyond it (a reply landing exactly on the boundary is on time). */
    const std::int64_t cutoff = static_cast<std::int64_t>(action_epoch) + kExpectationWindowSeconds;
    if (now > cutoff) {
        return JournalExpectationState::Expired;
    }
    return JournalExpectationState::Pending;
}

ResolutionReport resolve_expectations(const std::filesystem::path &journal_path,
                                      std::int64_t now, std::string_view now_rfc3339) {
    const JournalContents journal = load_journal(journal_path);
    ResolutionReport report;
    const std::uint64_t epoch = now > 0 ? static_cast<std::uint64_t>(now) : 0u;
    for (const JournalAction &action : journal.actions) {
        const JournalExpectationState state =
            derive_expectation_state(action, journal.events, now);
        if (state == JournalExpectationState::None) {
            continue;
        }
        ++report.evaluated;
        if (state == JournalExpectationState::Pending) {
            ++report.pending;
            continue;
        }
        bool prior_same = false;
        bool prior_different = false;
        for (const JournalResolution &resolution : journal.resolutions) {
            if (resolution.action_id != action.id) {
                continue;
            }
            if (resolution.state == state) {
                prior_same = true;
            } else {
                prior_different = true;
            }
        }
        if (prior_same) {
            ++report.skipped_already;
            continue;
        }
        JournalResolution resolution;
        resolution.action_id = action.id;
        resolution.state = state;
        resolution.at_epoch = epoch;
        resolution.at = std::string(now_rfc3339);
        append_journal_resolution(journal_path, resolution);
        switch (state) {
        case JournalExpectationState::Met:
            ++report.met_written;
            break;
        case JournalExpectationState::Unmet:
            ++report.unmet_written;
            break;
        case JournalExpectationState::Expired:
            ++report.expired_written;
            break;
        default:
            break;
        }
        if (prior_different) {
            ++report.state_changed;
        }
    }
    return report;
}

} // namespace atperson