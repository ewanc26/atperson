#include "atperson/graph.hpp"

#include <cassert>
#include <filesystem>

int main() {
    atperson::LanguageGraph graph;
    graph.observe("hello world hello", "at://example/post");

    const auto stats = graph.stats();
    assert(stats.node_count == 2u);
    assert(stats.edge_count == 2u);

    const auto associations = graph.associations("hello", 4u);
    assert(associations.size() == 1u);
    assert(associations.front().token == "world");

    const std::filesystem::path path = "atperson-cpp-test.bin";
    graph.save(path);
    auto restored = atperson::LanguageGraph::load(path);
    assert(restored.stats().node_count == stats.node_count);
    std::filesystem::remove(path);

    return 0;
}
