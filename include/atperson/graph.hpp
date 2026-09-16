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
    [[nodiscard]] std::vector<Association> associations(std::string_view token,
                                                        std::size_t limit = 10) const;
    void save(const std::filesystem::path &path) const;

    /**
     * Learn from one observation and return whether it was remembered as an
     * episode (the C-core selection policy). `ledger_id` links the memory to
     * its source entry in the observation ledger.
     */
    bool remember(std::string_view text, std::string_view source_id, std::string_view author_did,
                  std::uint64_t observed_at, std::uint64_t content_digest,
                  std::uint32_t schema_version, std::uint64_t ledger_id);

    /**
     * Recall episodes overlapping the query, strongest first. Recalled
     * episodes get their recall counters bumped, so this is not const.
     */
    [[nodiscard]] std::vector<atp_episode> recall(std::string_view query, std::uint64_t at_epoch,
                                                  std::size_t limit = 10);

    /** All remembered episodes in insertion order. */
    [[nodiscard]] std::vector<atp_episode> episodes() const;

    /**
     * Mirrored observation ledger: the entries the graph was trained from.
     * Mirrors are persisted in the snapshot so state can be rebuilt from
     * either the ledger or the snapshot alone.
     */
    void record_ledger_entry(const atp_ledger_entry &entry);
    [[nodiscard]] std::vector<atp_ledger_entry> ledger_entries() const;

  private:
    explicit LanguageGraph(atp_graph *graph) noexcept;
    atp_graph *graph_{};
};

} // namespace atperson

#endif
