#include "cli/neural.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "lock.hpp"
#include "resource/runtime.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Fixture {
    std::filesystem::path dir;
    std::filesystem::path model;
    std::filesystem::path ledger;
    std::vector<std::filesystem::path> resources;
};

atp_neural_architecture source_architecture() {
    atp_neural_architecture architecture{};
    architecture.version = ATPERSON_NEURAL_ARCHITECTURE_VERSION;
    architecture.embedding_dim = 32u;
    architecture.input_dim = 64u;
    architecture.hidden_layer_count = 1u;
    architecture.hidden_widths[0] = 64u;
    architecture.output_dim = 1u;
    return architecture;
}

atperson::ResourceOverrides expansion_overrides() {
    atperson::ResourceOverrides overrides;
    overrides.neural_embedding_dim = 48u;
    overrides.neural_hidden_layer_count = 1u;
    overrides.neural_hidden_widths =
        std::array<std::size_t, 3u>{96u, 0u, 0u};
    return overrides;
}

atperson::ResourceOverrides smaller_overrides() {
    atperson::ResourceOverrides overrides;
    overrides.neural_embedding_dim = 32u;
    overrides.neural_hidden_layer_count = 1u;
    overrides.neural_hidden_widths =
        std::array<std::size_t, 3u>{64u, 0u, 0u};
    return overrides;
}

std::vector<unsigned char> read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void cleanup(const std::filesystem::path &dir) {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

Fixture make_fixture(const std::filesystem::path &root,
                     std::string_view name) {
    Fixture fixture;
    fixture.dir = root / name;
    fixture.model = fixture.dir / "model.bin";
    fixture.ledger = fixture.dir / "ledger.bin";
    fixture.resources = {fixture.dir, fixture.model, fixture.ledger};

    cleanup(fixture.dir);
    std::filesystem::create_directories(fixture.dir);

    atperson::Ledger ledger(fixture.ledger);
    const std::string text = "alpha beta gamma";
    std::uint64_t id = 0u;
    const auto result = ledger.append(
        "at://neural-expand/1", "did:plc:expand-test", 100u,
        atperson::Ledger::digest(text), ATPERSON_SCHEMA_VERSION,
        ATP_LEDGER_OUTCOME_LEARNED, text, &id);
    assert(result == atperson::LedgerResult::New);
    assert(id == 1u);
    assert(ledger.last_id() == 1u);

    atp_graph_config config = atp_graph_default_config();
    config.seed = UINT64_C(0x123456789abcdef0);
    atperson::LanguageGraph graph(config, source_architecture());
    graph.observe(text, "at://neural-expand/1");
    graph.save(fixture.model);
    return fixture;
}

atperson::RuntimeResourceStatus status_for(
    const Fixture &fixture, const atperson::ResourceOverrides &overrides) {
    atp_graph_stats no_graph{};
    return atperson::inspect_runtime_resources(
        no_graph, fixture.resources, overrides);
}

bool architecture_equal(const atp_neural_architecture &left,
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

void assert_expanded(const Fixture &fixture,
                     std::uint64_t *seed_out = nullptr) {
    atperson::LanguageGraph graph =
        atperson::LanguageGraph::load(fixture.model);
    const auto history = graph.neural_migrations();
    assert(history.size() == 1u);
    assert(history[0].version == ATPERSON_NEURAL_MIGRATION_VERSION);
    assert(history[0].ledger_boundary_id == 1u);
    assert(history[0].seed != 0u);
    assert(architecture_equal(history[0].source, source_architecture()));

    const atp_neural_architecture active = graph.neural_architecture();
    assert(active.embedding_dim == 48u);
    assert(active.input_dim == 96u);
    assert(active.hidden_layer_count == 1u);
    assert(active.hidden_widths[0] == 96u);
    assert(architecture_equal(active, history[0].target));
    if (seed_out) {
        *seed_out = history[0].seed;
    }
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "atperson-neural-expand-test";
    cleanup(root);
    std::filesystem::create_directories(root);

    const auto overrides = expansion_overrides();
    Fixture first = make_fixture(root, "first");
    Fixture second = make_fixture(root, "second");

    std::ostringstream first_output;
    const auto first_status = status_for(first, overrides);
    assert(atperson::cli::run_neural_expand(
               first_output, first_status, first.dir, first.ledger, first.model,
               first.resources, overrides) == 0);
    assert(first_output.str().find("ledger boundary 1") != std::string::npos);
    assert(first_output.str().find("migration history entries: 1") !=
           std::string::npos);

    std::ostringstream second_output;
    const auto second_status = status_for(second, overrides);
    assert(atperson::cli::run_neural_expand(
               second_output, second_status, second.dir, second.ledger,
               second.model, second.resources, overrides) == 0);

    std::uint64_t first_seed = 0u;
    std::uint64_t second_seed = 0u;
    assert_expanded(first, &first_seed);
    assert_expanded(second, &second_seed);
    assert(first_seed == second_seed);
    assert(read_bytes(first.model) == read_bytes(second.model));

    /* Repeating the same proposal is an explicit no-op, not a second history
     * entry or a new seed. */
    const auto expanded_bytes = read_bytes(first.model);
    std::ostringstream noop_output;
    const auto noop_status = status_for(first, overrides);
    assert(atperson::cli::run_neural_expand(
               noop_output, noop_status, first.dir, first.ledger, first.model,
               first.resources, overrides) == 0);
    assert(noop_output.str().find("no mutation") != std::string::npos);
    assert(read_bytes(first.model) == expanded_bytes);
    assert_expanded(first);

    /* A smaller proposal is refused before the candidate snapshot can replace
     * model.bin. */
    const auto smaller = smaller_overrides();
    const auto refusal_status = status_for(first, smaller);
    bool refused = false;
    try {
        std::ostringstream ignored;
        (void)atperson::cli::run_neural_expand(
            ignored, refusal_status, first.dir, first.ledger, first.model,
            first.resources, smaller);
    } catch (const std::runtime_error &error) {
        refused = std::string(error.what()).find("refused") !=
                  std::string::npos;
    }
    assert(refused);
    assert(read_bytes(first.model) == expanded_bytes);

    /* The durable ledger is mandatory because its final id is the migration
     * boundary recorded into v7. */
    Fixture missing_ledger = make_fixture(root, "missing-ledger");
    std::filesystem::remove(missing_ledger.ledger);
    bool missing_refused = false;
    try {
        std::ostringstream ignored;
        const auto status = status_for(missing_ledger, overrides);
        (void)atperson::cli::run_neural_expand(
            ignored, status, missing_ledger.dir, missing_ledger.ledger,
            missing_ledger.model, missing_ledger.resources, overrides);
    } catch (const std::runtime_error &error) {
        missing_refused =
            std::string(error.what()).find("durable observation ledger") !=
            std::string::npos;
    }
    assert(missing_refused);

    /* Expansion is a writer mutation and refuses a concurrent writer. */
    Fixture locked = make_fixture(root, "locked");
    bool lock_refused = false;
    {
        atperson::StateLock held(locked.dir);
        try {
            std::ostringstream ignored;
            const auto status = status_for(locked, overrides);
            (void)atperson::cli::run_neural_expand(
                ignored, status, locked.dir, locked.ledger, locked.model,
                locked.resources, overrides);
        } catch (const atperson::StateLockError &) {
            lock_refused = true;
        }
    }
    assert(lock_refused);

    cleanup(root);
    return 0;
}
