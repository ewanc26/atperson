#ifndef ATPERSON_GRAPH_HPP
#define ATPERSON_GRAPH_HPP

#include "atperson/action.h"
#include "atperson/action.hpp"
#include "atperson/context.h"
#include "atperson/conversation.hpp"
#include "atperson/core.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

class Ledger;

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
    [[nodiscard]] std::vector<ActionCandidate> action_candidates(std::string_view context,
                                                                 std::size_t limit = 10) const;

    /** Thin read-only wrappers over the authoritative C23 planning APIs. */
    [[nodiscard]] std::vector<atp_action_plan>
    action_plans(std::string_view context,
                 atp_action_plan_config config = atp_action_plan_default_config()) const;
    [[nodiscard]] atp_action_decision
    action_decide(std::string_view context,
                  atp_action_decision_config config = atp_action_decision_default_config()) const;
    [[nodiscard]] atp_context_selection
    select_context(const atp_context_request &request,
                   atp_context_config config = atp_context_default_config()) const;

    void save(const std::filesystem::path &path) const;

    /**
     * Apply runtime-only graph growth ceilings. Existing nodes/edges are never
     * evicted when the ceilings shrink; the C core rejects only future growth.
     * These values are deployment policy and are deliberately not persisted.
     */
    void set_capacity(std::size_t node_capacity_max, std::size_t edge_capacity_max) noexcept;

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
     * Slowly learned familiarity score for `token` (exponentially weighted
     * exposure), 0.0 when the token is unknown. Read-only.
     */
    [[nodiscard]] float familiarity(std::string_view token) const noexcept;

    /**
     * Record one explicit valence event (action outcome, interaction,
     * approach/avoidance) for a known token. Throws std::runtime_error on
     * failure; ATP_ERR_NOT_FOUND means the token has never been observed —
     * valence attaches to experienced subjects only. See docs/valence.md.
     */
    void valence_event(std::string_view token, atp_valence_kind kind, float signal,
                       std::uint64_t at_epoch, std::string_view source_id);

    /**
     * Valence state for `token`, or std::nullopt when the token is unknown or
     * has never received an event (neutral, not a fake zero). Read-only.
     */
    [[nodiscard]] std::optional<atp_valence_state> valence(std::string_view token) const;

    /** All valence records in vocabulary order. Read-only. */
    [[nodiscard]] std::vector<atp_valence_state> valence_records() const;

    /**
     * Mirrored observation ledger: the entries the graph was trained from.
     * Mirrors are persisted in the snapshot so state can be rebuilt from
     * either the ledger or the snapshot alone.
     */
    void record_ledger_entry(const atp_ledger_entry &entry);
    /** Record with conversational context (issue #24). `context` fields
     * longer than ATPERSON_CONTEXT_URI_BYTES-1 throw. */
    void record_ledger_entry(const atp_ledger_entry &entry,
                             const ConversationContext &context);
    [[nodiscard]] std::vector<atp_ledger_entry> ledger_entries() const;

    /** Conversational context of the i-th mirrored entry (issue #24). */
    [[nodiscard]] atp_conversation_context ledger_context(std::size_t index) const;

    /**
     * Deterministic replay: re-apply every committed observation in `ledger`
     * to this graph in ledger id order. Pass a freshly constructed graph
     * for a rebuild. Throws on the first entry that cannot be replayed
     * (payload-less LEARNED entry, incompatible schema version, digest
     * mismatch); the graph is left partially trained, so callers discard it
     * on failure. No network access.
     */
    atp_replay_report replay(const Ledger &ledger);

  private:
    explicit LanguageGraph(atp_graph *graph) noexcept;
    atp_graph *graph_{};
};

} // namespace atperson

#endif
