#include "atperson/bootstrap.h"
#include "atperson/graph.hpp"
#include "action_inspection.hpp"
#include "audit/command.hpp"
#include "cli/config.hpp"
#include "cli/cursor.hpp"
#include "cli/graph_inspection.hpp"
#include "cli/ingest.hpp"
#include "cli/ledger_maintenance.hpp"
#include "cli/sync.hpp"
#include "cli/usage.hpp"
#include "resource_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void print_stats(const atperson::LanguageGraph &graph) {
    const auto stats = graph.stats();
    std::cout << "observations: " << stats.observations << '\n'
              << "token observations: " << stats.token_observations << '\n'
              << "nodes: " << stats.node_count << '\n'
              << "edges: " << stats.edge_count << '\n'
              << "training steps: " << stats.training_steps << '\n'
              << "mean loss: " << std::fixed << std::setprecision(6) << stats.mean_loss << '\n'
              << "episodes: " << stats.episode_count << " (capacity " << stats.episode_capacity
              << ", evictions " << stats.episode_evictions << ")\n";
}

atperson::LanguageGraph load_or_create(const std::filesystem::path &path) {
    if (std::filesystem::exists(path)) {
        return atperson::LanguageGraph::load(path);
    }
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    return atperson::LanguageGraph();
}

/* Usage as a callback for command atoms that report argument errors. */
void usage(std::ostream &out) {
    atperson::cli::print_usage(out);
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }

        char home_buffer[4096];
        if (atp_default_home_directory(home_buffer, sizeof(home_buffer), nullptr) != nullptr) {
            char notice[ATP_BOOTSTRAP_NOTICE_BYTES];
            if (atp_bootstrap_home(home_buffer, notice, sizeof(notice)) == ATP_OK &&
                notice[0] != '\0') {
                std::cerr << "atperson: " << notice << '\n';
            }
        }

        const std::string_view command = argv[1];
        const auto path = atperson::cli::state_path();
        const auto resource_paths = atperson::cli::durable_paths();
        const auto resource_overrides = atperson::resource_overrides_from_environment();
        const atp_graph_stats no_graph_stats{};
        auto resource_status = atperson::inspect_runtime_resources(
            no_graph_stats, resource_paths, resource_overrides);

        /* These commands operate only on ledger/runtime metadata. Avoid loading
         * a potentially large model when it cannot contribute to the result. */
        if (command == "rebuild") {
            return atperson::cli::run_rebuild(
                std::cout, resource_status, atperson::cli::data_dir(),
                atperson::cli::ledger_path(), path, resource_paths, resource_overrides,
                print_stats);
        }

        if (command == "compact") {
            return atperson::cli::run_compact(std::cout, resource_status,
                                              atperson::cli::data_dir(),
                                              atperson::cli::ledger_path());
        }

        if (command == "withdraw") {
            if (argc < 4) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_withdraw(std::cout, resource_status,
                                              atperson::cli::data_dir(),
                                              atperson::cli::ledger_path(), argv[2], argv[3],
                                              usage);
        }

        if (command == "cursor") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            if (sub != "status" && sub != "reset") {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_cursor(std::cout, resource_status,
                                            atperson::cli::data_dir(),
                                            atperson::cli::ingestion_state_path(), sub);
        }

        if (std::filesystem::exists(path)) {
            std::error_code size_error;
            const auto snapshot_bytes = std::filesystem::file_size(path, size_error);
            if (!size_error) {
                atperson::require_runtime_snapshot_headroom(resource_status, snapshot_bytes);
            }
        }

        auto graph = load_or_create(path);
        resource_status =
            atperson::refresh_runtime_resources(graph, resource_paths, resource_overrides);

        if (command == "resources") {
            atperson::print_runtime_resources(std::cout, resource_status);
            return 0;
        }

        if (command == "stats") {
            print_stats(graph);
            return 0;
        }

        if (atperson::is_action_inspection_command(command)) {
            std::vector<std::string_view> arguments;
            arguments.reserve(argc > 2 ? static_cast<std::size_t>(argc - 2) : 0u);
            for (int i = 2; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            return atperson::run_action_inspection_command(
                std::cout, graph, command, arguments,
                static_cast<std::uint64_t>(std::time(nullptr)));
        }

        if (command == "audit") {
            std::vector<std::string_view> arguments;
            arguments.reserve(argc > 2 ? static_cast<std::size_t>(argc - 2) : 0u);
            for (int i = 2; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            return atperson::audit::run_audit_command(std::cout, graph, command, arguments);
        }

        if (command == "ingest") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_ingest(std::cout, resource_status,
                                            atperson::cli::data_dir(), graph, path, argv[2],
                                            argc >= 4 ? argv[3] : nullptr, print_stats);
        }

        if (command == "ingest-file") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_ingest_file(std::cout, resource_status,
                                                 atperson::cli::data_dir(), graph, path,
                                                 argv[2], argc >= 4 ? argv[3] : nullptr,
                                                 print_stats);
        }

        if (command == "assoc") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_assoc(std::cout, resource_status, graph, argv[2],
                                            argc >= 4 ? argv[3] : nullptr);
        }

        if (command == "candidates") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_candidates(std::cout, resource_status, graph, argv[2],
                                                 argc >= 4 ? argv[3] : nullptr);
        }

        if (command == "familiarity") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_familiarity(std::cout, graph, argv[2]);
        }

        if (command == "recall") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_recall(
                std::cout, resource_status, graph, argv[2], argc >= 4 ? argv[3] : nullptr,
                static_cast<std::uint64_t>(std::time(nullptr)));
        }

        if (command == "sync") {
            const int max_pages = argc >= 3 ? atperson::cli::parse_limit(argv[2], 1) : 1;
            return atperson::cli::run_sync(std::cout, std::cerr, resource_status,
                                          atperson::cli::data_dir(), graph, path,
                                          atperson::cli::ledger_path(),
                                          atperson::cli::ingestion_state_path(), max_pages,
                                          print_stats);
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "atperson: " << error.what() << '\n';
        return 1;
    }
}
