// Ledger maintenance command atoms: rebuild, compact, withdraw.
//
// Implementation of the contracts in ledger.hpp. Every body here
// is moved byte-faithfully from the original single-file dispatch in
// src/app/main.cpp; behaviour, ordering and output text are unchanged.

#include "ledger.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "journal/store.hpp"
#include "lock.hpp"
#include "resource/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {
namespace cli {
namespace {

std::uint64_t migration_seed_mix(std::uint64_t hash,
                                 std::uint64_t value) noexcept {
    constexpr std::uint64_t FNV_PRIME = UINT64_C(1099511628211);
    for (unsigned byte = 0u; byte < 8u; ++byte) {
        hash ^= value & UINT64_C(0xff);
        hash *= FNV_PRIME;
        value >>= 8u;
    }
    return hash;
}

std::uint64_t mix_architecture(std::uint64_t hash,
                               const atp_neural_architecture &architecture) noexcept {
    hash = migration_seed_mix(hash, architecture.version);
    hash = migration_seed_mix(hash, architecture.embedding_dim);
    hash = migration_seed_mix(hash, architecture.input_dim);
    hash = migration_seed_mix(hash, architecture.hidden_layer_count);
    for (std::size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        hash = migration_seed_mix(hash, architecture.hidden_widths[i]);
    }
    return migration_seed_mix(hash, architecture.output_dim);
}

/*
 * Migration seeds are persisted in v7, so replay never derives them again.
 * This derivation only chooses the seed for a new operator mutation. It uses
 * durable generation facts so retrying the same interrupted mutation yields
 * the same new-coordinate initialisation without touching the graph RNG.
 */
std::uint64_t derive_migration_seed(
    const LanguageGraph &graph, std::uint64_t ledger_boundary,
    const atp_neural_architecture &source,
    const atp_neural_architecture &target) noexcept {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    hash = migration_seed_mix(hash, UINT64_C(0x4154504552534f4e)); /* ATPERSON */
    hash = migration_seed_mix(hash, ATPERSON_NEURAL_MIGRATION_VERSION);
    hash = migration_seed_mix(hash, ledger_boundary);
    hash = migration_seed_mix(hash, graph.neural_migrations().size());

    const atp_graph_stats stats = graph.stats();
    hash = migration_seed_mix(hash, stats.observations);
    hash = migration_seed_mix(hash, stats.token_observations);
    hash = migration_seed_mix(hash, stats.training_steps);
    hash = mix_architecture(hash, source);
    hash = mix_architecture(hash, target);
    return hash == 0u ? UINT64_C(0x6e657572616c7631) : hash;
}

bool same_architecture(const atp_neural_architecture &left,
                       const atp_neural_architecture &right) {
    if (left.version != right.version ||
        left.embedding_dim != right.embedding_dim ||
        left.input_dim != right.input_dim ||
        left.hidden_layer_count != right.hidden_layer_count ||
        left.output_dim != right.output_dim) {
        return false;
    }
    for (std::size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
        if (left.hidden_widths[i] != right.hidden_widths[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

int run_rebuild(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    const std::filesystem::path &data_dir, const std::filesystem::path &ledger_file,
    const std::filesystem::path &model_path,
    const std::vector<std::filesystem::path> &resource_paths,
    const ResourceOverrides &resource_overrides,
    const std::filesystem::path &journal_file,
    const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to rebuild from");
    }
    atperson::Ledger ledger(ledger_file);

    /*
     * Recover generation metadata instead of replanning topology from current
     * hardware. For an expanded v7 generation the active ARCH descriptor is
     * the final topology, while replay must begin at the first migration's
     * source and cross every persisted ledger boundary in order.
     */
    atp_neural_architecture final_architecture{};
    atp_neural_architecture initial_architecture{};
    std::vector<atp_neural_migration> migrations;
    if (std::filesystem::exists(model_path)) {
        const atp_status probe = atp_snapshot_neural_architecture(
            model_path.string().c_str(), &final_architecture);
        if (probe != ATP_OK) {
            throw std::runtime_error(
                "rebuild: cannot read the persisted neural architecture from " +
                model_path.string() + ": " + atp_status_string(probe));
        }
        atperson::require_runtime_neural_headroom(
            resource_status, final_architecture, 0u, 0u);

        std::error_code size_error;
        const std::uint64_t snapshot_bytes =
            std::filesystem::file_size(model_path, size_error);
        if (!size_error) {
            atperson::require_runtime_snapshot_headroom(resource_status,
                                                        snapshot_bytes);
        }

        /* v7 history is validated by the snapshot loader. Scope the loaded
         * generation tightly: only its durable migration metadata is needed
         * for rebuild; learned bytes are reconstructed from the ledger. */
        {
            atperson::LanguageGraph persisted =
                atperson::LanguageGraph::load(model_path);
            migrations = persisted.neural_migrations();
        }
        initial_architecture =
            migrations.empty() ? final_architecture : migrations.front().source;
    } else {
        final_architecture =
            atperson::neural_architecture_of(resource_status.budget.neural);
        initial_architecture = final_architecture;
        atperson::require_runtime_neural_headroom(
            resource_status, final_architecture, 0u, 0u);
    }

    atperson::LanguageGraph rebuilt(atp_graph_default_config(),
                                    initial_architecture);

    /*
     * Growth ceilings are derived against the final persisted width, not the
     * smaller initial topology. Otherwise replay could admit a graph that is
     * safe before expansion but unsafe once wider per-node embeddings land.
     */
    RuntimeResourceStatus rebuild_resources;
    rebuild_resources.system = atperson::probe_system_resources(resource_paths);
    rebuild_resources.budget = atperson::derive_resource_budget(
        rebuild_resources.system, rebuilt.stats(), resource_overrides,
        &final_architecture);
    rebuilt.set_capacity(rebuild_resources.budget.node_capacity_max,
                         rebuild_resources.budget.edge_capacity_max);
    atperson::require_runtime_write_headroom(rebuild_resources);

    const auto report = migrations.empty()
                            ? rebuilt.replay(ledger)
                            : rebuilt.replay(ledger, migrations);
    if (!same_architecture(rebuilt.neural_architecture(),
                           final_architecture)) {
        throw std::runtime_error(
            "rebuild: replay completed at a neural topology different from "
            "the persisted generation");
    }
    const auto rebuilt_stats = rebuilt.stats();
    atperson::require_runtime_neural_headroom(
        rebuild_resources, final_architecture, rebuilt_stats.node_count,
        rebuilt_stats.edge_count);

    /* Replay the journal's explicit valence events after the ledger so the
     * rebuilt state includes experience-derived valence. The journal is the
     * authority for self-authored experience; the ledger is the authority for
     * third-party observation. Replay order is deterministic: ledger entries
     * in id order, then journal valence entries in append order. */
    const JournalContents journal = atperson::load_journal(journal_file);
    for (const JournalValence &entry : journal.valence) {
        const std::optional<atp_valence_kind> kind = valence_kind_from_name(entry.kind);
        if (!kind.has_value()) {
            throw std::runtime_error("journal valence entry has unknown kind '" +
                                     entry.kind + "'");
        }
        rebuilt.valence_event(entry.token, kind.value(), entry.signal,
                              entry.at_epoch, entry.source);
    }

    rebuilt.save(model_path);
    out << "replayed " << report.replayed << " observation(s) from the ledger"
        << " (mirrored " << report.mirrored << " skipped, excluded "
        << report.excluded_pending << " pending, " << report.excluded_failed
        << " failed, " << report.excluded_withdrawn << " withdrawn";
    if (report.migrations_applied != 0u) {
        out << ", applied " << report.migrations_applied
            << " neural migration(s)";
    }
    out << ")\n";
    out << "replayed " << journal.valence.size() << " valence event(s) from the journal\n";
    print_stats(rebuilt);
    return 0;
}

int run_neural_expand(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    const std::filesystem::path &data_dir,
    const std::filesystem::path &ledger_file,
    const std::filesystem::path &model_path,
    const std::vector<std::filesystem::path> &resource_paths,
    const ResourceOverrides &resource_overrides) {
    require_runtime_write_headroom(resource_status);
    const StateLock writer_lock(data_dir);

    if (!std::filesystem::exists(model_path)) {
        throw std::runtime_error(
            "neural expand requires an existing persisted model generation");
    }
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error(
            "neural expand requires the durable observation ledger at " +
            ledger_file.string());
    }

    /*
     * Work on a separately loaded candidate. The caller's read-only status
     * graph, if any, is never mutated. atp_graph_save itself writes a sibling
     * temporary image and atomically renames it, so model.bin changes only
     * after the complete expanded v7 snapshot has been encoded and synced.
     */
    std::error_code size_error;
    const std::uint64_t snapshot_bytes =
        std::filesystem::file_size(model_path, size_error);
    if (!size_error) {
        require_runtime_snapshot_headroom(resource_status, snapshot_bytes);
    }

    atp_neural_architecture persisted_architecture{};
    const atp_status probe = atp_snapshot_neural_architecture(
        model_path.string().c_str(), &persisted_architecture);
    if (probe != ATP_OK) {
        throw std::runtime_error(
            "neural expand: cannot read persisted neural architecture from " +
            model_path.string() + ": " + atp_status_string(probe));
    }
    require_runtime_neural_headroom(resource_status, persisted_architecture,
                                    0u, 0u);

    LanguageGraph candidate = LanguageGraph::load(model_path);
    RuntimeResourceStatus locked_status =
        refresh_runtime_resources(candidate, resource_paths, resource_overrides);
    require_runtime_write_headroom(locked_status);

    const NeuralExpansionPlan plan =
        plan_neural_expansion(candidate, locked_status);
    if (plan.status == NeuralExpansionStatus::at_recommendation) {
        out << "neural expand: no mutation; " << plan.reason << '\n';
        return 0;
    }
    if (plan.status != NeuralExpansionStatus::available) {
        throw std::runtime_error("neural expand refused: " + plan.reason);
    }
    if (!plan.fits_current_headroom) {
        throw std::runtime_error(
            "neural expand refused: proposed topology does not fit current "
            "safe memory headroom");
    }

    const atp_graph_stats before = candidate.stats();
    require_runtime_neural_headroom(
        locked_status, plan.proposed, before.node_count, before.edge_count);

    Ledger ledger(ledger_file);
    const std::uint64_t ledger_boundary = ledger.last_id();
    const std::uint64_t seed = derive_migration_seed(
        candidate, ledger_boundary, plan.active, plan.proposed);

    const atp_neural_migration migration{
        .version = ATPERSON_NEURAL_MIGRATION_VERSION,
        .seed = seed,
        .ledger_boundary_id = ledger_boundary,
        .source = plan.active,
        .target = plan.proposed,
    };

    candidate.expand_neural(migration);
    if (!same_architecture(candidate.neural_architecture(), plan.proposed)) {
        throw std::runtime_error(
            "neural expand: core migration completed at an unexpected topology");
    }

    const atp_graph_stats after = candidate.stats();
    require_runtime_neural_headroom(
        locked_status, plan.proposed, after.node_count, after.edge_count);
    require_runtime_write_headroom(locked_status);

    candidate.save(model_path);

    out << "neural expand: migrated generation at ledger boundary "
        << ledger_boundary << '\n'
        << "migration version: " << migration.version << '\n'
        << "migration seed: " << migration.seed << '\n'
        << "embedding: " << plan.active.embedding_dim << " -> "
        << plan.proposed.embedding_dim << '\n'
        << "shared parameters: " << plan.active_parameter_count << " -> "
        << plan.proposed_parameter_count << '\n'
        << "migration history entries: "
        << candidate.neural_migrations().size() << '\n';
    return 0;
}

int run_compact(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &ledger_file) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to compact");
    }
    atperson::Ledger ledger(ledger_file);
    const auto report = ledger.compact();
    out << "compacted " << report.entries << " entr"
        << (report.entries == 1u ? "y" : "ies") << " ("
        << report.patches_flattened << " patch record(s) flattened, "
        << report.payloads_dropped << " withdrawn payload(s) dropped)\n"
        << "ledger " << report.bytes_before << " -> " << report.bytes_after
        << " bytes\n";
    return 0;
}

int run_withdraw(std::ostream &out, const RuntimeResourceStatus &resource_status,
                 const std::filesystem::path &data_dir,
                 const std::filesystem::path &ledger_file, std::string_view scope,
                 std::string_view target,
                 const std::function<void(std::ostream &)> &usage) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to withdraw from");
    }
    atperson::Ledger ledger(ledger_file);
    if (scope == "id") {
        const std::uint64_t id = std::strtoull(std::string(target).c_str(), nullptr, 10);
        ledger.withdraw(id);
        out << "withdrew observation " << id << "\n";
    } else if (scope == "source") {
        const std::size_t withdrawn = ledger.withdraw_source(target);
        out << "withdrew " << withdrawn << " observation(s) from " << target << '\n';
    } else if (scope == "author") {
        const std::size_t withdrawn = ledger.withdraw_author(target);
        out << "withdrew " << withdrawn << " observation(s) by " << target << '\n';
    } else {
        usage(std::cerr);
        return 2;
    }
    out << "run `atperson rebuild` to apply the withdrawal to learned state\n";
    return 0;
}

} // namespace cli
} // namespace atperson
