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
    /**
     * Create a fresh graph at a caller-validated neural architecture
     * (issue #73). The runtime derives `architecture` from its capacity
     * recommendation via neural_architecture_of, which guarantees the C-core
     * layout validation passes; passing an invalid descriptor from elsewhere
     * throws std::bad_alloc with a NULL core graph, matching the behaviour of
     * the legacy constructor.
     */
    LanguageGraph(atp_graph_config config, const atp_neural_architecture &architecture);
    ~LanguageGraph();

    LanguageGraph(const LanguageGraph &) = delete;
    LanguageGraph &operator=(const LanguageGraph &) = delete;

    LanguageGraph(LanguageGraph &&other) noexcept;
    LanguageGraph &operator=(LanguageGraph &&other) noexcept;

    static LanguageGraph load(const std::filesystem::path &path);

    void observe(std::string_view text, std::string_view source_id = {});
    [[nodiscard]] atp_graph_stats stats() const noexcept;
    [[nodiscard]] atp_neural_architecture neural_architecture() const;
    [[nodiscard]] atp_neural_architecture_report neural_report() const;
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

    /**
     * Evidence-gated recall. `config` selects the gate policy (NULL = default
     * eager behaviour); `report`, when non-NULL, receives the scan/gate
     * evidence for the call.
     */
    [[nodiscard]] std::vector<atp_episode> recall(std::string_view query, std::uint64_t at_epoch,
                                                  const atp_recall_config *config,
                                                  atp_recall_report *report, std::size_t limit);

    /** All remembered episodes in insertion order. */
    [[nodiscard]] std::vector<atp_episode> episodes() const;

    /** Derived episode groups (issue #55), ordered by first member. */
    [[nodiscard]] std::vector<atp_episode_group> episode_groups() const;

    /** Ledger ids of the episodes in `group_id`, in insertion order. */
    [[nodiscard]] std::vector<std::uint64_t> episode_group_members(std::uint32_t group_id) const;

    /** Plasticity report for online scorer parameter protection (issue #59). */
    [[nodiscard]] atp_plasticity_report plasticity_report() const;

    /**
     * Slowly learned familiarity score for `token` (exponentially weighted
     * exposure), 0.0 when the token is unknown. Read-only.
     */
    [[nodiscard]] float familiarity(std::string_view token) const noexcept;

    /**
     * True when `token` is in the graph's vocabulary (observed at least
     * once). Read-only; never interns. Callers honouring the valence
     * contract (experienced subjects only) check this before applying an
     * event.
     */
    [[nodiscard]] bool has_token(std::string_view token) const noexcept;

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
