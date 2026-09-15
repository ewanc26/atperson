#ifndef ATPERSON_GRAPH_HPP
#define ATPERSON_GRAPH_HPP

#include "atperson/core.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

struct Association {
    std::string token;
    float score{};
    std::uint64_t observations{};
    std::uint64_t last_source_hash{};
};

class LanguageGraph {
  public:
    LanguageGraph();
    explicit LanguageGraph(atp_graph_config config);
    ~LanguageGraph();

    LanguageGraph(const LanguageGraph &) = delete;
    LanguageGraph &operator=(const LanguageGraph &) = delete;

    LanguageGraph(LanguageGraph &&other) noexcept;
    LanguageGraph &operator=(LanguageGraph &&other) noexcept;

    static LanguageGraph load(const std::filesystem::path &path);

    void observe(std::string_view text, std::string_view source_id = {});
    [[nodiscard]] atp_graph_stats stats() const noexcept;
    [[nodiscard]] std::vector<Association>
    associations(std::string_view token, std::size_t limit = 10) const;
    void save(const std::filesystem::path &path) const;

  private:
    explicit LanguageGraph(atp_graph *graph) noexcept;
    atp_graph *graph_{};
};

} // namespace atperson

#endif
