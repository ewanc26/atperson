#include "atperson/graph.hpp"

#include <stdexcept>
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

void LanguageGraph::save(const std::filesystem::path &path) const {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    require(atp_graph_save(graph_, path.string().c_str()), "save language graph");
}

void LanguageGraph::record_ledger_entry(const atp_ledger_entry &entry) {
    require(atp_graph_add_ledger_entry(graph_, &entry), "record ledger entry");
}

std::vector<atp_ledger_entry> LanguageGraph::ledger_entries() const {
    std::vector<atp_ledger_entry> result(atp_graph_ledger_count(graph_));
    for (std::size_t i = 0; i < result.size(); ++i) {
        require(atp_graph_ledger_entry(graph_, i, &result[i]), "read ledger mirror");
    }
    return result;
}

} // namespace atperson
