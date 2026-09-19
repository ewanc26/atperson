#include "atperson/bootstrap.h"
#include "atperson/graph.hpp"
#include "inspection.hpp"
#include "audit/command.hpp"
#include "cli/config.hpp"
#include "cli/control.hpp"
#include "cli/cursor.hpp"
#include "cli/daemon.hpp"
#include "cli/graph.hpp"
#include "cli/ingest.hpp"
#include "cli/jetstream.hpp"
#include "cli/ledger.hpp"
#include "cli/neural.hpp"
#include "cli/outbound.hpp"
#include "cli/publish.hpp"
#include "cli/sync.hpp"
#include "cli/usage.hpp"
#include "control/state.hpp"
#include "journal/command.hpp"
#include "runtime.hpp"

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

        /*
         * Neural expansion is a state mutation, so it acquires the writer lock
         * before loading the candidate generation. Status remains a read-only
         * command below.
         */
        if (command == "neural" && argc >= 3 &&
            std::string_view(argv[2]) == "expand") {
            return atperson::cli::run_neural_expand(
                std::cout, resource_status, atperson::cli::data_dir(),
                atperson::cli::ledger_path(), path, resource_paths,
                resource_overrides);
        }

        /* These commands operate only on ledger/runtime metadata. Avoid loading
         * a potentially large model when it cannot contribute to the result. */
        if (command == "rebuild") {
            return atperson::cli::run_rebuild(
                std::cout, resource_status, atperson::cli::data_dir(),
                atperson::cli::ledger_path(), path, resource_paths, resource_overrides,
                atperson::cli::action_journal_path(), print_stats);
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

        if (command == "control") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            const std::string argument = argc >= 4 ? argv[3] : "";
            return atperson::cli::run_control(std::cout, resource_status,
                                              atperson::cli::control_state_path(), sub,
                                              argument);
        }

        if (command == "outbound") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            std::vector<std::string_view> arguments;
            arguments.reserve(argc > 3 ? static_cast<std::size_t>(argc - 3) : 0u);
            for (int i = 3; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            return atperson::cli::run_outbound_command(
                std::cout, atperson::cli::outbound_policy_path(),
                atperson::cli::outbound_budget_path(), atperson::cli::control_state_path(),
                sub, arguments, static_cast<std::int64_t>(std::time(nullptr)));
        }

        if (command == "publish") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return atperson::cli::run_publish(
                std::cout, atperson::cli::data_dir(),
                atperson::cli::outbound_policy_path(), atperson::cli::outbound_budget_path(),
                atperson::cli::control_state_path(), atperson::cli::outbound_audit_path(),
                atperson::cli::action_journal_path(),
                argv[2], static_cast<std::int64_t>(std::time(nullptr)));
        }

        if (command == "neural" && !std::filesystem::exists(path)) {
            throw std::runtime_error(
                "neural inspection requires an existing persisted model generation; "
                "there is nothing to inspect yet");
        }

        auto graph = atperson::load_or_create_graph(resource_status, path);
        resource_status =
            atperson::refresh_runtime_resources(graph, resource_paths, resource_overrides);

        if (command == "resources") {
            atperson::print_runtime_resources(std::cout, resource_status, graph,
                                               resource_overrides);
            return 0;
        }

        if (command == "neural") {
            const std::string_view sub = argc >= 3 ? argv[2] : "status";
            if (sub != "status") {
                usage(std::cerr);
                return 2;
            }
            const auto expansion =
                atperson::plan_neural_expansion(graph, resource_status);
            atperson::print_neural_expansion_plan(std::cout, expansion);
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

        if (command == "journal") {
            const std::string sub = argc >= 3 ? argv[2] : "actions";
            std::vector<std::string_view> arguments;
            arguments.reserve(argc > 3 ? static_cast<std::size_t>(argc - 3) : 0u);
            for (int i = 3; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            return atperson::journal::run_journal_command(
                std::cout, graph, atperson::cli::action_journal_path(),
                atperson::cli::data_dir(), path, sub, arguments.data(), arguments.size(),
                now, atperson::control_now_rfc3339());
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
            if (argc > 7) {
                std::cerr << "recall usage: recall <query> [limit] [min-overlap] "
                             "[max-prefilter] [on|off]\n";
                return 2;
            }
            return atperson::cli::run_recall(
                std::cout, resource_status, graph, argv[2], argc >= 4 ? argv[3] : nullptr,
                argc >= 5 ? argv[4] : nullptr, argc >= 6 ? argv[5] : nullptr,
                argc >= 7 ? argv[6] : nullptr, static_cast<std::uint64_t>(std::time(nullptr)));
        }

        if (command == "groups") {
            return atperson::cli::run_groups(std::cout, resource_status, graph);
        }

        if (command == "sync") {
            const int max_pages = argc >= 3 ? atperson::cli::parse_limit(argv[2], 1) : 1;
            return atperson::cli::run_sync(std::cout, std::cerr, resource_status,
                                          atperson::cli::data_dir(), graph, path,
                                          atperson::cli::ledger_path(),
                                          atperson::cli::ingestion_state_path(), max_pages,
                                          print_stats);
        }

        if (command == "daemon") {
            const int max_cycles =
                argc >= 3 ? atperson::cli::parse_limit(argv[2], 0) : 0;
            return atperson::cli::run_daemon_command(
                std::cout, std::cerr, resource_status, atperson::cli::data_dir(), graph,
                path, atperson::cli::ledger_path(), atperson::cli::ingestion_state_path(),
                max_cycles, print_stats);
        }

        if (command == "jetstream") {
            const int max_events =
                argc >= 3 ? atperson::cli::parse_limit(argv[2], 0) : 0;
            const int max_ms =
                argc >= 4 ? atperson::cli::parse_limit(argv[3], 0) : 0;
            return atperson::cli::run_jetstream(
                std::cout, resource_status, atperson::cli::data_dir(), graph, path,
                atperson::cli::ledger_path(), atperson::cli::ingestion_state_path(),
                max_events, max_ms, print_stats);
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "atperson: " << error.what() << '\n';
        return 1;
    }
}
