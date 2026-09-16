#include "action_inspection.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"

#include <cassert>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void record_learned(atperson::LanguageGraph &graph, std::uint64_t id,
                    std::string_view text, std::string_view source,
                    std::string_view author, std::uint64_t observed_at) {
    const std::uint64_t digest = atp_ledger_digest(text.data(), text.size());
    const bool remembered = graph.remember(text, source, author, observed_at, digest,
                                           ATPERSON_SCHEMA_VERSION, id);
    assert(remembered);

    atp_ledger_entry entry{};
    entry.id = id;
    entry.observed_at = observed_at;
    entry.content_digest = digest;
    entry.schema_version = ATPERSON_SCHEMA_VERSION;
    entry.outcome = ATP_LEDGER_OUTCOME_LEARNED;
    assert(source.size() < sizeof(entry.source_id));
    assert(author.size() < sizeof(entry.author_did));
    source.copy(entry.source_id, source.size());
    entry.source_id[source.size()] = '\0';
    author.copy(entry.author_did, author.size());
    entry.author_did[author.size()] = '\0';
    graph.record_ledger_entry(entry);
}

void assert_stats_equal(const atp_graph_stats &a, const atp_graph_stats &b) {
    assert(a.node_count == b.node_count);
    assert(a.edge_count == b.edge_count);
    assert(a.observations == b.observations);
    assert(a.token_observations == b.token_observations);
    assert(a.training_steps == b.training_steps);
    assert(a.episode_count == b.episode_count);
    assert(a.episode_evictions == b.episode_evictions);
    assert(a.capacity_rejections == b.capacity_rejections);
}

void test_plan_decision_and_context_render_authoritative_evidence() {
    atperson::LanguageGraph graph;
    record_learned(graph, 1u, "start mid", "at://did:plc:alice/app.bsky.feed.post/one",
                   "did:plc:alice", 100u);
    record_learned(graph, 2u, "mid tail", "at://did:plc:alice/app.bsky.feed.post/two",
                   "did:plc:alice", 200u);

    const atp_graph_stats before = graph.stats();

    std::ostringstream plans;
    assert(atperson::run_action_inspection_command(
               plans, graph, "plans", {"start", "4", "2"}, 300u) == 0);
    const std::string plans_text = plans.str();
    assert(plans_text.find("layer: learned-core") != std::string::npos);
    assert(plans_text.find("network-policy: not-evaluated") != std::string::npos);
    assert(plans_text.find("plan 0") != std::string::npos);
    assert(plans_text.find("step 0 token=mid") != std::string::npos);
    assert(plans_text.find("association=") != std::string::npos);
    assert(plans_text.find("support=") != std::string::npos);

    std::ostringstream decision;
    assert(atperson::run_action_inspection_command(
               decision, graph, "decide", {"start", "4", "2"}, 300u) == 0);
    const std::string decision_text = decision.str();
    assert(decision_text.find("outcome: plan") != std::string::npos);
    assert(decision_text.find("raw-plans:") != std::string::npos);
    assert(decision_text.find("viable-plans:") != std::string::npos);
    assert(decision_text.find("stop-evidence reason=") != std::string::npos);
    assert(decision_text.find("network-policy: not-evaluated") != std::string::npos);

    std::ostringstream context;
    assert(atperson::run_action_inspection_command(
               context, graph, "context",
               {"start", "at://did:plc:alice/app.bsky.feed.post/one", "did:plc:alice"},
               300u) == 0);
    const std::string context_text = context.str();
    assert(context_text.find("kind=immediate") != std::string::npos);
    assert(context_text.find("reason=immediate-input") != std::string::npos);
    assert(context_text.find("kind=memory") != std::string::npos);
    assert(context_text.find("recall total=") != std::string::npos);
    assert(context_text.find("kind=author-state") != std::string::npos);
    assert(context_text.find("identifier=did:plc:alice") != std::string::npos);
    assert(context_text.find("kind=source-state") != std::string::npos);
    assert(context_text.find("ledger=1") != std::string::npos);

    const atp_graph_stats after = graph.stats();
    assert_stats_equal(before, after);
}

void test_abstention_and_bounds_are_visible() {
    atperson::LanguageGraph graph;
    graph.observe("alpha beta", "at://inspect/alpha");

    std::ostringstream decision;
    assert(atperson::run_action_inspection_command(
               decision, graph, "decide", {"unknown"}, 100u) == 0);
    const std::string output = decision.str();
    assert(output.find("outcome: abstain") != std::string::npos);
    assert(output.find("abstain-reason: no-candidates") != std::string::npos);

    bool threw = false;
    try {
        std::ostringstream invalid;
        (void)atperson::run_action_inspection_command(
            invalid, graph, "plans", {"alpha", "17"}, 100u);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    std::ostringstream unknown;
    assert(atperson::run_action_inspection_command(
               unknown, graph, "not-an-inspection-command", {}, 100u) == 2);
}

} // namespace

int main() {
    test_plan_decision_and_context_render_authoritative_evidence();
    test_abstention_and_bounds_are_visible();
    return 0;
}
