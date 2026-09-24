#include "intent/sweep.hpp"

#include "intent/state.hpp"

#include <cstdint>
#include <string>

namespace atperson {

IntentSweepReport sweep_intents(const std::filesystem::path &journal_path, std::int64_t now,
                                std::string_view now_rfc3339) {
    IntentSweepReport report;
    const JournalContents journal = load_journal(journal_path);
    const std::uint64_t epoch = now > 0 ? static_cast<std::uint64_t>(now) : 0u;

    /* One id appears once, at its latest entry (the durable current record).
     * Terminal entries are appended to the same file, and because the file is
     * loaded before any append, two intents never influence each other. */
    std::vector<std::string> ids;
    for (const JournalIntent &entry : journal.intents) {
        bool present = false;
        for (const std::string &id : ids) {
            if (id == entry.id) {
                present = true;
                break;
            }
        }
        if (!present) {
            ids.push_back(entry.id);
        }
    }

    for (const std::string &id : ids) {
        const JournalIntent *latest = latest_intent(journal, id);
        if (latest == nullptr) {
            continue;
        }
        ++report.evaluated;
        if (latest->state != IntentState::Open) {
            ++report.already_terminal;
            continue;
        }
        const IntentState effective = derive_intent_state(*latest, now);
        if (effective == IntentState::Open) {
            ++report.open_kept;
            continue;
        }
        JournalIntent terminal = *latest;
        terminal.state = effective;
        terminal.at_epoch = epoch;
        terminal.at = std::string(now_rfc3339);
        append_journal_intent(journal_path, terminal);
        if (effective == IntentState::Expired) {
            ++report.expired_written;
        } else {
            ++report.closed_written;
        }
    }
    return report;
}

} // namespace atperson