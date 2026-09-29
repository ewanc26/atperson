#include "journal/trace.hpp"

#include <iomanip>
#include <set>
#include <string>

namespace atperson {

std::optional<ActionTrace> build_action_trace(const JournalContents &journal,
                                              std::string_view action_id) {
    ActionTrace trace;
    for (const JournalAction &action : journal.actions) {
        if (action.id == action_id) {
            trace.action = &action; /* a later attempt with the same id wins */
        }
    }
    if (trace.action == nullptr) {
        return std::nullopt;
    }

    std::set<std::string_view> cited;
    cited.insert(trace.action->id);
    if (!trace.action->uri.empty()) {
        cited.insert(trace.action->uri);
    }
    for (const JournalEvent &event : journal.events) {
        if (event.action_id == action_id) {
            trace.events.push_back(&event);
            cited.insert(event.event_uri);
        }
    }
    for (const JournalResolution &resolution : journal.resolutions) {
        if (resolution.action_id == action_id) {
            trace.resolution = &resolution; /* append-only: last entry is current */
        }
    }
    for (const JournalValence &valence : journal.valence) {
        if (cited.contains(valence.source)) {
            trace.valence.push_back(&valence);
        }
    }
    /* Intents are append-only too: only the last record per intent id is its
     * current state. */
    std::set<std::string_view> seen_intents;
    for (auto it = journal.intents.rbegin(); it != journal.intents.rend(); ++it) {
        if (!seen_intents.insert(it->id).second) {
            continue;
        }
        for (const std::string &member : it->actions) {
            if (member == action_id) {
                trace.intents.push_back(&*it);
                break;
            }
        }
    }
    return trace;
}

void render_action_trace(std::ostream &out, const ActionTrace &trace) {
    const JournalAction &action = *trace.action;
    out << "layer: journal (self-authored provenance)\n";
    out << "action: " << action.id << "\n";
    out << "  kind: " << action.kind << "\n";
    out << "  attempted: " << action.at << "\n";
    out << "  outcome: " << journal_action_outcome_name(action.outcome)
        << " (reason: " << action.reason << ")\n";
    out << "  approval-digest: " << action.digest << "\n";
    if (!action.uri.empty()) {
        out << "  uri: " << action.uri << "\n";
    }
    if (!action.cid.empty()) {
        out << "  cid: " << action.cid << "\n";
    }
    out << "  text: " << action.text << "\n";
    if (action.expectation.has_value()) {
        out << "  expectation: " << action.expectation->kind << " reply-likelihood="
            << std::fixed << std::setprecision(2) << action.expectation->reply_likelihood
            << " tokens=";
        for (std::size_t i = 0u; i < action.expectation->tokens.size(); ++i) {
            out << (i == 0u ? "" : ",") << action.expectation->tokens[i];
        }
        out << "\n";
    } else {
        out << "  expectation: none recorded\n";
    }

    out << "resolution: ";
    if (trace.resolution != nullptr) {
        out << journal_expectation_state_name(trace.resolution->state) << " at "
            << trace.resolution->at << "\n";
    } else {
        out << "none recorded\n";
    }

    out << "events: " << trace.events.size() << "\n";
    for (const JournalEvent *event : trace.events) {
        out << "  " << event->at << " " << event->via << " " << event->author_did << " "
            << event->event_uri << "\n";
    }

    out << "valence: " << trace.valence.size() << "\n";
    for (const JournalValence *valence : trace.valence) {
        out << "  " << valence->at << " " << valence->token << " " << valence->kind << " "
            << std::fixed << std::setprecision(3) << valence->signal << " source="
            << valence->source;
        if (!valence->provenance.empty()) {
            out << " provenance=" << valence->provenance;
        }
        out << "\n";
    }

    out << "intents: " << trace.intents.size() << "\n";
    for (const JournalIntent *intent : trace.intents) {
        out << "  thread=" << intent->id << " " << intent_state_name(intent->state)
            << " actions=" << intent->actions.size() << " responder=" << intent->responder
            << " expires=" << intent->expires_at << "\n";
    }
}

} // namespace atperson
