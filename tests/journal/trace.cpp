/* Per-action trace: joins exactly the journal entries that belong to one
 * action (its events, latest resolution, citing valence updates and the
 * intents it is part of), ignores everything else, reports a missing action as
 * absent, and renders deterministically. Offline. */

#include "journal/store.hpp"
#include "journal/trace.hpp"

#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

using namespace atperson;

JournalAction make_action(std::string id, std::string uri) {
    JournalAction action;
    action.id = std::move(id);
    action.kind = "reply";
    action.text = "the quiet harbour";
    action.digest = "0123456789abcdef";
    action.outcome = JournalActionOutcome::Executed;
    action.reason = "executed";
    action.uri = std::move(uri);
    action.cid = "bafycid";
    action.at = "2026-09-17T10:00:00Z";
    return action;
}

JournalContents fixture() {
    JournalContents journal;
    JournalAction first = make_action("aaa", "at://did:plc:me/app.bsky.feed.post/aaa");
    first.expectation = JournalExpectation{"approach", 0.7f, {"quiet", "harbour"}};
    journal.actions.push_back(first);
    journal.actions.push_back(make_action("bbb", "at://did:plc:me/app.bsky.feed.post/bbb"));

    journal.events.push_back({"aaa", "at://did:plc:x/app.bsky.feed.post/e1", "did:plc:x",
                              "parent", "2026-09-18T10:00:00Z"});
    journal.events.push_back({"bbb", "at://did:plc:y/app.bsky.feed.post/e2", "did:plc:y",
                              "quote", "2026-09-18T11:00:00Z"});

    journal.resolutions.push_back({"aaa", JournalExpectationState::Unmet, 1u, "t1"});
    journal.resolutions.push_back({"aaa", JournalExpectationState::Met, 2u, "t2"});

    /* Cited three ways (id, executed URI, event URI) plus one unrelated. */
    journal.valence.push_back({"quiet", "approach", 0.5f, "aaa", 1u, "t", ""});
    journal.valence.push_back(
        {"harbour", "action", 0.25f, "at://did:plc:me/app.bsky.feed.post/aaa", 2u, "t", "map:r1"});
    journal.valence.push_back(
        {"moon", "interaction", 0.5f, "at://did:plc:x/app.bsky.feed.post/e1", 3u, "t", ""});
    journal.valence.push_back({"other", "avoid", -0.5f, "bbb", 4u, "t", ""});

    JournalIntent open;
    open.id = "at://root/1";
    open.actions = {"aaa"};
    open.state = IntentState::Open;
    JournalIntent closed = open;
    closed.actions = {"aaa", "ccc"};
    closed.state = IntentState::Closed;
    JournalIntent unrelated;
    unrelated.id = "at://root/2";
    unrelated.actions = {"bbb"};
    journal.intents = {open, closed, unrelated};
    return journal;
}

} // namespace

int main() {
    const JournalContents journal = fixture();

    assert(!build_action_trace(journal, "missing").has_value());

    const auto trace = build_action_trace(journal, "aaa");
    assert(trace.has_value());
    assert(trace->action->id == "aaa");
    assert(trace->events.size() == 1u && trace->events[0]->via == "parent");
    /* Append-only: the last resolution is the current one. */
    assert(trace->resolution != nullptr && trace->resolution->state == JournalExpectationState::Met);
    assert(trace->valence.size() == 3u);
    /* Only the latest record of an intent, and only intents naming the action. */
    assert(trace->intents.size() == 1u);
    assert(trace->intents[0]->state == IntentState::Closed && trace->intents[0]->actions.size() == 2u);

    const auto other = build_action_trace(journal, "bbb");
    assert(other.has_value());
    assert(other->events.size() == 1u && other->resolution == nullptr);
    assert(other->valence.size() == 1u && other->valence[0]->token == "other");
    assert(other->action->expectation.has_value() == false);

    std::ostringstream first;
    std::ostringstream second;
    render_action_trace(first, *trace);
    render_action_trace(second, *trace);
    assert(first.str() == second.str());
    const std::string text = first.str();
    assert(text.find("layer: journal (self-authored provenance)") != std::string::npos);
    assert(text.find("outcome: executed (reason: executed)") != std::string::npos);
    assert(text.find("approval-digest: 0123456789abcdef") != std::string::npos);
    assert(text.find("expectation: approach reply-likelihood=0.70 tokens=quiet,harbour") !=
           std::string::npos);
    assert(text.find("resolution: met at t2") != std::string::npos);
    assert(text.find("provenance=map:r1") != std::string::npos);
    assert(text.find("events: 1") != std::string::npos);

    std::ostringstream bare;
    render_action_trace(bare, *other);
    assert(bare.str().find("expectation: none recorded") != std::string::npos);
    assert(bare.str().find("resolution: none recorded") != std::string::npos);

    std::puts("journal trace tests passed");
    return 0;
}
