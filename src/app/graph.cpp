#include "atperson/graph.hpp"

#include "atperson/action.h"
#include "atperson/ledger.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace atperson {

namespace {

void require(atp_status status, std::string_view operation) {
    if (status == ATP_OK) {
        return;
    }
    throw std::runtime_error(std::string(operation) + ": " + atp_status_string(status));
}

} // namespace

LanguageGraph::LanguageGraph() : LanguageGraph(atp_graph_default_config()) {}

LanguageGraph::LanguageGraph(atp_graph_config config) : graph_(atp_graph_create(&config)) {
    if (!graph_) {
        throw std::bad_alloc();
    }
}

LanguageGraph::LanguageGraph(atp_graph_config config, const atp_neural_architecture &architecture)
    : graph_(atp_graph_create_with_architecture(&config, &architecture)) {
    if (!graph_) {
        throw std::bad_alloc();
    }
}

LanguageGraph::LanguageGraph(atp_graph *graph) noexcept : graph_(graph) {}

LanguageGraph::~LanguageGraph() {
    atp_graph_destroy(graph_);
}

LanguageGraph::LanguageGraph(LanguageGraph &&other) noexcept
    : graph_(std::exchange(other.graph_, nullptr)) {}

LanguageGraph &LanguageGraph::operator=(LanguageGraph &&other) noexcept {
    if (this != &other) {
        atp_graph_destroy(graph_);
        graph_ = std::exchange(other.graph_, nullptr);
    }
    return *this;
}

LanguageGraph LanguageGraph::load(const std::filesystem::path &path) {
    atp_status status = ATP_OK;
    atp_graph *graph = atp_graph_load(path.string().c_str(), &status);
    if (!graph) {
        if (status == ATP_ERR_SCHEMA) {
            throw std::runtime_error(
                "load language graph: the snapshot was trained under a learning schema "
                "this build cannot extend; rebuild from the ledger with `atperson rebuild` "
                "or start a new model generation");
        }
        require(status, "load language graph");
        throw std::runtime_error("load language graph: unknown failure");
    }
    return LanguageGraph(graph);
}

void LanguageGraph::observe(std::string_view text, std::string_view source_id) {
    const std::string owned_text(text);
    const std::string owned_source(source_id);
    require(atp_graph_observe_text(graph_, owned_text.c_str(), owned_source.c_str()),
            "observe text");
}

atp_graph_stats LanguageGraph::stats() const noexcept {
    return atp_graph_get_stats(graph_);
}

atp_neural_architecture LanguageGraph::neural_architecture() const {
    atp_neural_architecture architecture{};
    require(atp_graph_neural_architecture(graph_, &architecture), "read neural architecture");
    return architecture;
}

atp_neural_architecture_report LanguageGraph::neural_report() const {
    atp_neural_architecture_report report{};
    require(atp_graph_neural_report(graph_, &report), "read neural architecture report");
    return report;
}

std::vector<Association> LanguageGraph::associations(std::string_view token,
                                                     std::size_t limit) const {
    if (limit == 0u) {
        return {};
    }

    std::vector<atp_association> raw(limit);
    std::size_t count = 0u;
    const std::string owned_token(token);
    const atp_status status =
        atp_graph_associations(graph_, owned_token.c_str(), raw.data(), raw.size(), &count);
    if (status == ATP_ERR_NOT_FOUND) {
        return {};
    }
    require(status, "query associations");

    std::vector<Association> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(Association{
            .token = raw[i].token,
            .score = raw[i].score,
            .observations = raw[i].observations,
            .last_source_hash = raw[i].last_source_hash,
        });
    }
    return result;
}

std::vector<ActionCandidate> LanguageGraph::action_candidates(std::string_view context,
                                                              std::size_t limit) const {
    if (limit == 0u) {
        return {};
    }

    std::vector<atp_action_candidate> raw(limit);
    std::size_t count = 0u;
    const std::string owned_context(context);
    require(atp_graph_action_candidates(graph_, owned_context.c_str(), raw.data(), raw.size(),
                                        &count),
            "query action candidates");

    std::vector<ActionCandidate> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(ActionCandidate{
            .token = raw[i].token,
            .score = raw[i].score,
            .association_score = raw[i].association_score,
            .familiarity_score = raw[i].familiarity_score,
            .support_score = raw[i].support_score,
            .supporting_observations = raw[i].supporting_observations,
            .context_matches = raw[i].context_matches,
        });
    }
    return result;
}

std::vector<atp_action_plan> LanguageGraph::action_plans(std::string_view context,
                                                        atp_action_plan_config config) const {
    std::array<atp_action_plan, ATPERSON_PLAN_MAX_BEAM_WIDTH> raw{};
    std::size_t count = 0u;
    const std::string owned_context(context);
    require(atp_graph_action_plans(graph_, owned_context.c_str(), &config, raw.data(), raw.size(),
                                   &count),
            "query action plans");
    return std::vector<atp_action_plan>(raw.begin(), raw.begin() + count);
}

atp_action_decision LanguageGraph::action_decide(std::string_view context,
                                                 atp_action_decision_config config) const {
    const std::string owned_context(context);
    atp_action_decision decision{};
    require(atp_graph_action_decide(graph_, owned_context.c_str(), &config, &decision),
            "query action decision");
    return decision;
}

atp_context_selection LanguageGraph::select_context(const atp_context_request &request,
                                                    atp_context_config config) const {
    atp_context_selection selection{};
    require(atp_graph_select_context(graph_, &request, &config, &selection),
            "select planner context");
    return selection;
}

void LanguageGraph::save(const std::filesystem::path &path) const {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    require(atp_graph_save(graph_, path.string().c_str()), "save language graph");
}

void LanguageGraph::set_capacity(std::size_t node_capacity_max,
                                 std::size_t edge_capacity_max) noexcept {
    atp_graph_set_capacity(graph_, node_capacity_max, edge_capacity_max);
}

void LanguageGraph::record_ledger_entry(const atp_ledger_entry &entry) {
    require(atp_graph_add_ledger_entry(graph_, &entry), "record ledger entry");
}

void LanguageGraph::record_ledger_entry(const atp_ledger_entry &entry,
                                        const ConversationContext &context) {
    const atp_conversation_context raw = to_c_conversation_context(context);
    require(atp_graph_add_ledger_entry_with_context(graph_, &entry, &raw),
            "record ledger entry");
}

atp_conversation_context LanguageGraph::ledger_context(std::size_t index) const {
    atp_conversation_context context = {};
    require(atp_graph_ledger_context(graph_, index, &context), "read ledger context");
    return context;
}

std::vector<atp_ledger_entry> LanguageGraph::ledger_entries() const {
    std::vector<atp_ledger_entry> result(atp_graph_ledger_count(graph_));
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(atp_graph_ledger_entry(graph_, i, &result[i]), "read ledger mirror");
    }
    return result;
}

bool LanguageGraph::remember(std::string_view text, std::string_view source_id,
                             std::string_view author_did, std::uint64_t observed_at,
                             std::uint64_t content_digest, std::uint32_t schema_version,
                             std::uint64_t ledger_id) {
    const std::string owned_text(text);
    const std::string owned_source(source_id);
    const std::string owned_author(author_did);
    bool remembered = false;
    require(atp_graph_observe_with_memory(graph_, owned_text.c_str(), owned_source.c_str(),
                                          owned_author.c_str(), observed_at, content_digest,
                                          schema_version, ledger_id, &remembered),
            "remember observation");
    return remembered;
}

std::vector<atp_episode> LanguageGraph::recall(std::string_view query, std::uint64_t at_epoch,
                                               std::size_t limit) {
    return recall(query, at_epoch, nullptr, nullptr, limit);
}

std::vector<atp_episode> LanguageGraph::recall(std::string_view query, std::uint64_t at_epoch,
                                               const atp_recall_config *config,
                                               atp_recall_report *report, std::size_t limit) {
    if (limit == 0u) {
        return {};
    }

    std::vector<atp_episode> result(limit);
    std::size_t count = 0u;
    const std::string owned_query(query);
    require(atp_graph_recall(graph_, owned_query.c_str(), at_epoch, config, report,
                             result.data(), result.size(), &count),
            "recall episodes");
    result.resize(count);
    return result;
}

std::vector<atp_episode> LanguageGraph::episodes() const {
    std::vector<atp_episode> result(atp_graph_episode_count(graph_));
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(atp_graph_episode_at(graph_, i, &result[i]), "read episode");
    }
    return result;
}

std::vector<atp_episode_group> LanguageGraph::episode_groups() const {
    std::size_t count = 0u;
    require(atp_graph_episode_groups(graph_, nullptr, 0u, &count), "count episode groups");
    std::vector<atp_episode_group> result(count);
    require(atp_graph_episode_groups(graph_, result.data(), result.size(), &count),
            "read episode groups");
    result.resize(count);
    return result;
}

std::vector<std::uint64_t> LanguageGraph::episode_group_members(std::uint32_t group_id) const {
    std::size_t count = 0u;
    require(atp_graph_episode_group_members(graph_, group_id, nullptr, 0u, &count),
            "count episode group members");
    std::vector<std::uint64_t> result(count);
    require(atp_graph_episode_group_members(graph_, group_id, count ? result.data() : nullptr,
                                            result.size(), &count),
            "read episode group members");
    result.resize(count);
    return result;
}

atp_plasticity_report LanguageGraph::plasticity_report() const {
    atp_plasticity_report report{};
    require(atp_graph_plasticity_report(graph_, &report), "read plasticity report");
    return report;
}

float LanguageGraph::familiarity(std::string_view token) const noexcept {
    const std::string owned_token(token);
    return atp_graph_familiarity(graph_, owned_token.c_str());
}

bool LanguageGraph::has_token(std::string_view token) const noexcept {
    const std::string owned_token(token);
    return atp_graph_has_token(graph_, owned_token.c_str());
}

void LanguageGraph::valence_event(std::string_view token, atp_valence_kind kind, float signal,
                                  std::uint64_t at_epoch, std::string_view source_id) {
    const std::string owned_token(token);
    const std::string owned_source(source_id);
    require(atp_graph_valence_event(graph_, owned_token.c_str(), kind, signal, at_epoch,
                                     owned_source.c_str()),
            "record valence event");
}

std::optional<atp_valence_state> LanguageGraph::valence(std::string_view token) const {
    const std::string owned_token(token);
    atp_valence_state state{};
    const atp_status status = atp_graph_valence(graph_, owned_token.c_str(), &state);
    if (status == ATP_ERR_NOT_FOUND) {
        return std::nullopt;
    }
    require(status, "query valence");
    return state;
}

std::vector<atp_valence_state> LanguageGraph::valence_records() const {
    std::vector<atp_valence_state> result(atp_graph_valence_count(graph_));
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(atp_graph_valence_at(graph_, i, &result[i]), "read valence record");
    }
    return result;
}

atp_replay_report LanguageGraph::replay(const Ledger &ledger) {
    atp_replay_report report = {};
    const atp_status status = atp_replay_ledger(ledger.handle(), graph_, &report);
    if (status == ATP_ERR_SCHEMA) {
        throw std::runtime_error(
            "replay ledger: entry " + std::to_string(report.failed_at_id) +
            " was recorded under learning schema " + std::to_string(report.failed_schema) +
            ", which this build cannot replay; start a new model generation or compact "
            "the ledger under the current schema");
    }
    require(status, "replay ledger");
    return report;
}

atp_replay_report LanguageGraph::rebuild_from_ledger(const Ledger &ledger) {
    LanguageGraph rebuilt(atp_graph_default_config(), neural_architecture());
    const atp_replay_report report = rebuilt.replay(ledger);
    *this = std::move(rebuilt);
    return report;
}

} // namespace atperson
