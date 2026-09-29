#ifndef ATPERSON_JOURNAL_TRACE_HPP
#define ATPERSON_JOURNAL_TRACE_HPP

// Per-action decision-cycle trace (the "what happened" half of the agent-loop
// WorkTrace contract): for one self-authored action, everything the journal
// durably knows about it, joined by stable identifiers.
//
// Read-only and derived. It joins existing journal entries — the frozen action
// (what was executed and with which approval digest), the later public events
// that referenced it, its recorded expectation resolution, the explicit valence
// updates that cite it (its own id, its executed URI or one of its events), and
// the pending-intent conversations it belongs to. It reads no graph, no model
// and no network, and it never writes. A missing kind of evidence is simply
// absent from the trace; nothing is inferred to fill the gap.

#include "journal/store.hpp"

#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

namespace atperson {

struct ActionTrace {
    const JournalAction *action{};
    std::vector<const JournalEvent *> events;
    /* The latest recorded terminal state, if the resolution pass wrote one. */
    const JournalResolution *resolution{};
    std::vector<const JournalValence *> valence;
    /* The latest record of each intent whose conversation includes the action. */
    std::vector<const JournalIntent *> intents;
};

/* The trace for the action with this id, or nullopt when the journal has no
 * such action. Pointers borrow from `journal`, which must outlive the trace. */
[[nodiscard]] std::optional<ActionTrace> build_action_trace(const JournalContents &journal,
                                                            std::string_view action_id);

/* Deterministic, line-oriented rendering. */
void render_action_trace(std::ostream &out, const ActionTrace &trace);

} // namespace atperson

#endif
