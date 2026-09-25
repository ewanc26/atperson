#include "reconstruct.hpp"

#include "atperson/ledger.hpp"
#include "journal/store.hpp"
#include "publish.hpp"
#include "thought/store.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace atperson {
namespace {

/* Observation rkeys are base-32 of the ledger id (see publish.cpp); the
 * listing order is rkey order, so sort by id for replay order. */
std::uint64_t rkey_to_id(const std::string &rkey) {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::uint64_t id = 0u;
    for (char c : rkey) {
        const char *position = std::strchr(alphabet, c);
        if (!position) {
            return 0u;
        }
        id = (id << 5) | static_cast<std::uint64_t>(position - alphabet);
    }
    return id;
}

} // namespace

ReconstructReport reconstruct_state(RecordSource &source,
                                    const std::filesystem::path &ledger_path,
                                    const std::filesystem::path &journal_path,
                                    const std::filesystem::path &thoughts_path) {
    if (std::filesystem::exists(ledger_path) || std::filesystem::exists(journal_path) ||
        std::filesystem::exists(thoughts_path)) {
        throw std::runtime_error("reconstruct: target state already exists at " +
                                 ledger_path.string() + " / " + journal_path.string() +
                                 " / " + thoughts_path.string() +
                                 " — reconstruct builds fresh state only");
    }

    ReconstructReport report;
    Ledger ledger(ledger_path);

    /* 1. Observations: fetch, verify digest, replay. Withdrawn records
     * are skipped — their experience is excluded by the withdrawal, and
     * the payload is gone from the source anyway. */
    const std::vector<std::string> observation_rkeys =
        source.list_records(kObservationCollection);
    std::vector<std::pair<std::uint64_t, ObservationRecord>> observations;
    observations.reserve(observation_rkeys.size());
    for (const std::string &rkey : observation_rkeys) {
        const std::optional<std::string> json = source.get_record(kObservationCollection, rkey);
        if (!json) {
            ++report.observations_failed;
            report.failures.push_back("observation rkey " + rkey + " vanished mid-reconstruct");
            continue;
        }
        try {
            ObservationRecord record = parse_observation_record(*json);
            observations.emplace_back(rkey_to_id(rkey), std::move(record));
        } catch (const RecordError &error) {
            ++report.records_corrupt;
            report.failures.push_back(std::string("corrupt observation: ") + error.what());
        }
    }
    std::sort(observations.begin(), observations.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    for (const auto &[id, record] : observations) {
        if (record.outcome == "withdrawn") {
            ++report.observations_skipped_withdrawn;
            continue;
        }
        const std::optional<std::string> content = source.fetch_content(record.source_id);
        if (!content) {
            ++report.observations_failed;
            report.failures.push_back("source unavailable for observation " +
                                     std::to_string(record.id));
            continue;
        }
        if (Ledger::digest(*content) != record.content_digest) {
            ++report.observations_failed;
            report.failures.push_back("digest mismatch for observation " +
                                     std::to_string(record.id));
            continue;
        }
        const atp_ledger_outcome outcome = *ledger_outcome_from_name(record.outcome);
        const LedgerResult result = ledger.append(
            record.source_id, record.author_did, record.observed_at, record.content_digest,
            record.schema_version, outcome, *content, record.context, nullptr);
        if (result == LedgerResult::ExistsCommitted) {
            /* Dedup: the same (source, digest) already replayed. Not an
             * error — the record set may overlap. */
            continue;
        }
        ++report.observations_replayed;
    }

    /* 2. Actions and valence: replay to the journal in id order. */
    const std::vector<std::string> action_rkeys = source.list_records(kActionCollection);
    for (const std::string &rkey : action_rkeys) {
        const std::optional<std::string> json = source.get_record(kActionCollection, rkey);
        if (!json) {
            ++report.records_corrupt;
            report.failures.push_back("action rkey " + rkey + " vanished mid-reconstruct");
            continue;
        }
        try {
            const ActionRecord record = parse_action_record(*json);
            JournalAction action;
            action.id = record.id;
            action.kind = record.kind;
            action.text = record.text;
            action.digest = record.digest;
            action.outcome = *journal_action_outcome_from_name(record.outcome);
            action.reason = record.reason;
            action.uri = record.uri;
            action.cid = record.cid;
            action.at = record.at;
            append_journal_action(journal_path, action);
            ++report.actions_replayed;
        } catch (const std::exception &error) {
            ++report.records_corrupt;
            report.failures.push_back(std::string("corrupt action: ") + error.what());
        }
    }

    const std::vector<std::string> valence_rkeys = source.list_records(kValenceCollection);
    for (const std::string &rkey : valence_rkeys) {
        const std::optional<std::string> json = source.get_record(kValenceCollection, rkey);
        if (!json) {
            ++report.records_corrupt;
            report.failures.push_back("valence rkey " + rkey + " vanished mid-reconstruct");
            continue;
        }
        try {
            const ValenceRecord record = parse_valence_record(*json);
            JournalValence valence;
            valence.token = record.token;
            valence.kind = record.kind;
            valence.signal = record.signal;
            valence.source = record.source;
            valence.at_epoch = record.at_epoch;
            valence.at = record.at;
            valence.provenance = record.provenance;
            append_journal_valence(journal_path, valence);
            ++report.valence_replayed;
        } catch (const std::exception &error) {
            ++report.records_corrupt;
            report.failures.push_back(std::string("corrupt valence: ") + error.what());
        }
    }

    /* 3. Thoughts: replay to the thought store in id order. The record
     * carries the full text (self-authored), so there is nothing to
     * re-fetch or verify — a corrupt record is reported and skipped,
     * never replayed. */
    const std::vector<std::string> thought_rkeys = source.list_records(kThoughtCollection);
    for (const std::string &rkey : thought_rkeys) {
        const std::optional<std::string> json = source.get_record(kThoughtCollection, rkey);
        if (!json) {
            report.failures.push_back("thought rkey " + rkey + " vanished mid-reconstruct");
            ++report.records_corrupt;
            continue;
        }
        try {
            const ThoughtRecord record = parse_thought_record(*json);
            if (record.id != rkey) {
                report.failures.push_back("thought rkey " + rkey + " carries id " + record.id);
                ++report.records_corrupt;
                continue;
            }
            Thought thought;
            thought.id = record.id;
            thought.kind = record.kind;
            thought.text = record.text;
            thought.about_uri = record.about_uri;
            thought.at = record.at;
            write_thought(thoughts_path, thought);
            ++report.thoughts_replayed;
        } catch (const std::exception &error) {
            ++report.records_corrupt;
            report.failures.push_back(std::string("corrupt thought: ") + error.what());
        }
    }

    /* 4. Intents (#161): replay to the journal in rkey order, restoring
     * pending conversations on a new host. The record mirrors the journal
     * line exactly, so the rebuilt journal is what a local one would have
     * been; a corrupt record is reported and skipped, never replayed. */
    const std::vector<std::string> intent_rkeys = source.list_records(kIntentCollection);
    for (const std::string &rkey : intent_rkeys) {
        const std::optional<std::string> json = source.get_record(kIntentCollection, rkey);
        if (!json) {
            report.failures.push_back("intent rkey " + rkey + " vanished mid-reconstruct");
            ++report.records_corrupt;
            continue;
        }
        try {
            const IntentRecord record = parse_intent_record(*json);
            if (record.id != rkey) {
                report.failures.push_back("intent rkey " + rkey + " carries id " + record.id);
                ++report.records_corrupt;
                continue;
            }
            const std::optional<IntentState> state = intent_state_from_name(record.state);
            if (!state.has_value()) {
                report.failures.push_back("intent rkey " + rkey + " carries unknown state '" +
                                          record.state + "'");
                ++report.records_corrupt;
                continue;
            }
            JournalIntent intent;
            intent.id = record.id;
            intent.actions = record.actions;
            intent.responder = record.responder;
            intent.expires_at_epoch = record.expires_at_epoch;
            intent.expires_at = record.expires_at;
            intent.max_continuations = record.max_continuations;
            intent.state = *state;
            intent.at_epoch = record.at_epoch;
            intent.at = record.at;
            append_journal_intent(journal_path, intent);
            ++report.intents_replayed;
        } catch (const std::exception &error) {
            ++report.records_corrupt;
            report.failures.push_back(std::string("corrupt intent: ") + error.what());
        }
    }

    return report;
}

} // namespace atperson
