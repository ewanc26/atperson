#include "cli/neural.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "lock.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>

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

std::uint64_t mix_architecture(
    std::uint64_t hash,
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
 * Seeds are persisted in v7, so replay never derives them again. This only
 * selects the seed for a new operator mutation. Durable generation facts make
 * a retry of the same interrupted mutation choose identical new coordinates.
 */
std::uint64_t derive_migration_seed(
    const LanguageGraph &graph, std::uint64_t ledger_boundary,
    const atp_neural_architecture &source,
    const atp_neural_architecture &target) {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    hash = migration_seed_mix(hash, UINT64_C(0x4154504552534f4e));
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
     * Work on a separately loaded candidate. atp_graph_save writes a sibling
     * temporary image and atomically renames it, so model.bin changes only
     * after the complete expanded v7 image has been encoded and synced.
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

} // namespace cli
} // namespace atperson
