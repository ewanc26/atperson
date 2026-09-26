#include "atperson/bootstrap.h"
#include "protocol.hpp"
#include "atperson/graph.hpp"
#include "autonomy/heartbeat.hpp"
#include "autonomy/run_state.hpp"
#include "inspection.hpp"
#include "audit/command.hpp"
#include "cli/config.hpp"
#include "cli/control.hpp"
#include "cli/control_remote.hpp"
#include "cli/envelope.hpp"
#include "cli/cursor.hpp"
#include "cli/daemon.hpp"
#include "cli/drives.hpp"
#include "cli/graph.hpp"
#include "cli/ingest.hpp"
#include "cli/profile.hpp"
#include "cli/jetstream.hpp"
#include "cli/ledger.hpp"
#include "cli/metrics.hpp"
#include "cli/neural.hpp"
#include "cli/outbound.hpp"
#include "cli/publish.hpp"
#include "cli/reconstruct.hpp"
#include "cli/statepub.hpp"
#include "cli/protocol_command.hpp"
#include "cli/sync.hpp"
#include "cli/thoughts.hpp"
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

const char *verification_name(atperson::protocol::Verification verification) {
    switch (verification) {
    case atperson::protocol::Verification::Verified:
        return "verified";
    case atperson::protocol::Verification::Rejected:
        return "rejected";
    case atperson::protocol::Verification::Unverified:
        return "unverified";
    }
    return "unknown";
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }

        const std::string_view command = argv[1];
        char home_buffer[4096];
        if (command != "protocol" &&
            atp_default_home_directory(home_buffer, sizeof(home_buffer), nullptr) != nullptr) {
            char notice[ATP_BOOTSTRAP_NOTICE_BYTES];
            if (atp_bootstrap_home(home_buffer, notice, sizeof(notice)) == ATP_OK &&
                notice[0] != '\0') {
                std::cerr << "atperson: " << notice << '\n';
            }
        }

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
            if (sub == "envelope") {
                std::vector<std::string_view> envelope_arguments;
                for (int i = 4; i < argc; ++i) {
                    envelope_arguments.emplace_back(argv[i]);
                }
                return atperson::cli::run_envelope_command(
                    std::cout, resource_status,
                    atperson::cli::authorization_envelopes_path(),
                    atperson::cli::outbound_policy_path(), argv[3], envelope_arguments,
                    static_cast<std::int64_t>(std::time(nullptr)));
            }
            if (sub == "remote") {
                /* Remote operator channel (#143). The argument is the rest
                 * of the line, so `emit approve <digest>` arrives intact. */
                std::string argument;
                for (int i = 4; i < argc; ++i) {
                    if (!argument.empty()) {
                        argument += ' ';
                    }
                    argument += argv[i];
                }
                /* argv[3] is the remote subcommand, and reading it
                 * unconditionally ran off the end of argv for a bare
                 * `control remote`. Default it, so the caller's usage
                 * error is what a user sees. */
                const char *remote_sub = argc >= 4 ? argv[3] : "";
                return atperson::cli::run_control_remote(std::cout, resource_status,
                                                         remote_sub, argument);
            }
            return atperson::cli::run_control(std::cout, resource_status,
                                              atperson::cli::control_state_path(), sub,
                                              argument);
        }

        if (command == "autonomy") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            if (sub == "health") {
                /* Supervisor surface (#143): machine-readable liveness.
                 * Exit codes: 0 healthy, 1 stale, 2 unreadable/never
                 * started — a watchdog restarts on non-zero. The
                 * staleness threshold defaults to 15 minutes and is
                 * overridable via ATPERSON_HEALTH_MAX_AGE_SECONDS. */
                const char *raw_max = std::getenv("ATPERSON_HEALTH_MAX_AGE_SECONDS");
                const std::int64_t max_age =
                    raw_max != nullptr && raw_max[0] != '\0' ? std::stoll(raw_max)
                                                             : 15 * 60;
                const auto report = atperson::autonomy_health(
                    atperson::cli::autonomy_heartbeat_path(),
                    static_cast<std::int64_t>(std::time(nullptr)), max_age);
                std::cout << "{\"health\":\"" << atperson::autonomy_health_name(report.health)
                          << "\"";
                if (report.heartbeat) {
                    std::cout << ",\"run_id\":\"" << report.heartbeat->run_id
                              << "\",\"cycle\":" << report.heartbeat->cycle
                              << ",\"beat_at\":\"" << report.heartbeat->beat_at
                              << "\",\"phase\":\"" << report.heartbeat->phase << "\"";
                }
                std::cout << ",\"detail\":\"" << report.detail << "\"}\n";
                switch (report.health) {
                case atperson::AutonomyHealth::Healthy: return 0;
                case atperson::AutonomyHealth::Stale: return 1;
                case atperson::AutonomyHealth::Unreadable: return 2;
                }
                return 2;
            }
            if (sub != "status") {
                usage(std::cerr);
                return 2;
            }
            const auto state = atperson::load_autonomy_run_state(
                atperson::cli::autonomy_run_state_path());
            std::cout << "run id: " << state.run_id << '\n'
                      << "phase: " << atperson::autonomy_phase_name(state.phase) << '\n'
                      << "checkpoint: " << state.checkpoint << '\n'
                      << "last at: " << state.last_at << '\n'
                      << "detail: " << state.detail << '\n';
            /* Pending scheduler proposals (#140): frozen documents awaiting
             * operator approval or execution. */
            std::error_code ec;
            std::size_t proposals = 0u;
            for (std::filesystem::directory_iterator
                     it(atperson::cli::scheduler_proposals_path(), ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".json") {
                    ++proposals;
                }
            }
            std::cout << "pending proposals: " << proposals << '\n';
            return 0;
        }

        /* Thought commands: a thought and a listing need only the thought
         * store, never the model. `reflect` needs the graph and is
         * dispatched after it loads. */
        if (command == "thought") {
            std::vector<std::string> text_parts;
            for (int i = 2; i < argc; ++i) {
                text_parts.emplace_back(argv[i]);
            }
            return atperson::cli::run_thought_record(
                std::cout, atperson::cli::data_dir(), text_parts,
                atperson::control_now_rfc3339());
        }

        if (command == "thoughts") {
            std::vector<std::string_view> arguments;
            for (int i = 2; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            return atperson::cli::run_thoughts_list(
                std::cout, atperson::cli::data_dir(), arguments.data(), arguments.size());
        }

        if (command == "metrics") {
            std::vector<std::string_view> arguments;
            for (int i = 2; i < argc; ++i) {
                arguments.emplace_back(argv[i]);
            }
            return atperson::cli::run_metrics_list(
                std::cout, atperson::cli::data_dir(), arguments.data(), arguments.size());
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
                atperson::cli::authorization_envelopes_path(),
                argv[2], static_cast<std::int64_t>(std::time(nullptr)));
        }

        if (command == "statepub") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            if (sub == "status") {
                return atperson::cli::run_statepub_status(
                    std::cout, std::cerr, resource_status, atperson::cli::data_dir(),
                    atperson::cli::ledger_path(), atperson::cli::action_journal_path(),
                    atperson::cli::data_dir() / "thoughts");
            }
            if (sub == "drain") {
                bool offline = false;
                for (int i = 3; i < argc; ++i) {
                    if (std::string_view(argv[i]) == "--offline") {
                        offline = true;
                    }
                }
                return atperson::cli::run_statepub_drain(
                    std::cout, std::cerr, resource_status, atperson::cli::data_dir(),
                    atperson::cli::ledger_path(), atperson::cli::action_journal_path(),
                    atperson::cli::data_dir() / "thoughts",
                    atperson::cli::control_state_path(), offline);
            }
            usage(std::cerr);
            return 2;
        }

        if (command == "reconstruct") {
            if (argc >= 4 && std::string_view(argv[2]) == "--into") {
                return atperson::cli::run_reconstruct(std::cout, std::cerr, argv[3]);
            }
            usage(std::cerr);
            return 2;
        }

        if (command == "protocol") {
            const std::string_view subcommand = argc >= 3 ? argv[2] : "status";
            if (subcommand == "resolve") {
                if (argc < 4) {
                    usage(std::cerr);
                    return 2;
                }
                return atperson::cli::run_protocol_resolve(
                    std::cout, std::cerr, argv[3],
                    atperson::cli::env_or("ATPERSON_SERVICE", "https://bsky.social").c_str());
            }
            if (subcommand == "explain") {
                if (argc < 4) {
                    usage(std::cerr);
                    return 2;
                }
                const atperson::protocol::EvidenceLedger ledger(
                    atperson::cli::protocol_ledger_path());
                const auto entries = ledger.entries();
                std::size_t matches = 0;
                for (const auto &entry : entries) {
                    const std::string handle_prefix = std::string(argv[3]) + "|";
                    const bool subject_match = entry.subject == argv[3];
                    const bool handle_match =
                        entry.kind == atperson::protocol::EvidenceKind::Identity &&
                        entry.event_type == "#identity" &&
                        entry.payload.starts_with(handle_prefix);
                    if (!subject_match && !handle_match) continue;
                    ++matches;
                    std::cout << "subject=" << entry.subject << " source=" << entry.source
                              << " event=" << entry.event_type
                              << " payload=" << entry.payload
                              << " sequence=" << entry.sequence
                              << " observed_at=" << entry.observed_at
                              << " verification=" << verification_name(entry.verification)
                              << " confidence=" << entry.confidence << '\n';
                }
                if (matches == 0) {
                    std::cout << "no protocol evidence for " << argv[3] << '\n';
                    return 1;
                }
                return 0;
            }
            if (subcommand == "oauth-plan") {
                if (argc < 4) {
                    usage(std::cerr);
                    return 2;
                }
                const auto plan = atperson::protocol::make_loopback_oauth_plan(
                    atperson::cli::env_or("ATPERSON_OAUTH_REDIRECT",
                                          "http://127.0.0.1:43127/callback"),
                    argv[3]);
                if (!plan) {
                    std::cerr << "protocol oauth-plan: redirect must be a valid loopback URI\n";
                    return 1;
                }
                std::cout << "redirect=" << plan->redirect_uri << " scope=" << plan->scope
                          << " permission=" << static_cast<int>(plan->permission)
                          << " loopback-only=" << (plan->loopback_only ? "yes" : "no")
                          << '\n';
                return 0;
            }
            if (subcommand == "oauth-metadata") {
                if (argc < 4) {
                    usage(std::cerr);
                    return 2;
                }
                const auto metadata = atperson::protocol::localhost_oauth_client_metadata(
                    atperson::cli::env_or("ATPERSON_OAUTH_REDIRECT",
                                          "http://127.0.0.1:43127/callback"),
                    argv[3]);
                if (!metadata) {
                    std::cerr << "protocol oauth-metadata: invalid loopback redirect\n";
                    return 1;
                }
                std::cout << "{\"client_id\":\"" << metadata->client_id
                          << "\",\"redirect_uris\":[\"" << metadata->redirect_uri
                          << "\"],\"grant_types\":[\"authorization_code\",\"refresh_token\"]"
                          << ",\"response_types\":[\"code\"],\"scope\":\""
                          << metadata->scope
                          << "\",\"token_endpoint_auth_method\":\"none\""
                          << ",\"application_type\":\"native\",\"dpop_bound_access_tokens\":true}\n";
                return 0;
            }
            if (subcommand != "status") {
                usage(std::cerr);
                return 2;
            }
            const atperson::protocol::EvidenceLedger ledger(
                atperson::cli::protocol_ledger_path());
            const auto entries = ledger.entries();
            std::cout << "protocol evidence: " << entries.size() << " entries\n";
            for (const auto &entry : entries) {
                std::cout << "source=" << entry.source << " event=" << entry.event_type
                          << " subject=" << entry.subject << " sequence=" << entry.sequence
                          << " observed_at=" << entry.observed_at
                          << " payload=" << entry.payload
                          << " verification=" << verification_name(entry.verification)
                          << " confidence=" << entry.confidence << '\n';
            }
            return 0;
        }

        if (command == "jetstream" && argc >= 3 &&
            std::string_view(argv[2]) == "status") {
            std::filesystem::path collections_file =
                atperson::cli::jetstream_collections_path();
            std::filesystem::path dids_file =
                atperson::cli::jetstream_dids_path();

            for (int i = 3; i < argc; ++i) {
                const std::string_view argument = argv[i];
                if (argument == "--collections") {
                    if (i + 1 >= argc) {
                        std::cerr << "jetstream status: --collections requires a file path\n";
                        return 2;
                    }
                    collections_file = argv[++i];
                    continue;
                }
                if (argument == "--dids") {
                    if (i + 1 >= argc) {
                        std::cerr << "jetstream status: --dids requires a file path\n";
                        return 2;
                    }
                    dids_file = argv[++i];
                    continue;
                }
                std::cerr << "jetstream status: unexpected argument " << argument << '\n';
                return 2;
            }

            return atperson::cli::run_jetstream_status(
                std::cout, atperson::cli::jetstream_state_path(),
                collections_file, dids_file);
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

        if (command == "reflect") {
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            return atperson::cli::run_reflect_command(
                std::cout, atperson::cli::data_dir(), atperson::cli::action_journal_path(),
                graph, now, atperson::control_now_rfc3339());
        }

        if (command == "selfeval") {
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            return atperson::cli::run_self_eval_command(
                std::cout, atperson::cli::data_dir(), atperson::cli::action_journal_path(),
                graph, now, atperson::control_now_rfc3339());
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

        if (command == "profile") {
            return atperson::cli::run_profile(std::cout, graph);
        }

        if (command == "drives") {
            const int count = argc >= 3 ? atperson::cli::parse_limit(argv[2], 8) : 8;
            return atperson::cli::run_drives_command(
                std::cout, graph, atperson::Ledger(atperson::cli::ledger_path()),
                atperson::cli::action_journal_path(),
                static_cast<std::int64_t>(std::time(nullptr)),
                static_cast<std::size_t>(count));
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
            /* "status" is handled above before the model is loaded. */
            if (argc >= 3 && std::string_view(argv[2]) == "archive") {
                const auto parse_sequence = [](const char *value,
                                               const char *name) -> std::uint64_t {
                    try {
                        std::size_t consumed = 0;
                        const std::string text(value);
                        const auto parsed = std::stoull(text, &consumed, 10);
                        if (consumed != text.size()) throw std::invalid_argument("trailing");
                        return parsed;
                    } catch (const std::exception &) {
                        throw std::runtime_error(std::string("jetstream archive: ") +
                                                 name + " must be a decimal sequence");
                    }
                };
                std::optional<std::uint64_t> after;
                std::optional<std::uint64_t> before;
                std::optional<std::uint64_t> span;
                std::filesystem::path collections_file =
                    atperson::cli::jetstream_collections_path();
                std::filesystem::path dids_file = atperson::cli::jetstream_dids_path();
                std::vector<std::uint64_t> positional;
                positional.reserve(2u);
                for (int i = 3; i < argc; ++i) {
                    const std::string_view argument = argv[i];
                    if (argument == "--collections" && i + 1 < argc) {
                        collections_file = argv[++i];
                    } else if (argument == "--dids" && i + 1 < argc) {
                        dids_file = argv[++i];
                    } else if (argument == "--span" && i + 1 < argc) {
                        span = parse_sequence(argv[++i], "--span");
                    } else if (argument.starts_with("--")) {
                        std::cerr << "jetstream archive: unknown or incomplete option "
                                  << argument << '\n';
                        return 2;
                    } else if (positional.size() < 2u) {
                        positional.push_back(
                            parse_sequence(argv[i],
                                           positional.empty() ? "after-seq" : "before-seq"));
                    } else {
                        std::cerr << "jetstream archive: too many positional arguments\n";
                        return 2;
                    }
                }
                if (span) {
                    if (positional.size() == 1u) before = positional[0];
                    if (positional.size() == 2u) after = positional[0], before = positional[1];
                } else {
                    if (!positional.empty()) after = positional[0];
                    if (positional.size() == 2u) before = positional[1];
                }
                if (after && span) {
                    std::cerr << "jetstream archive: after-seq and --span cannot be used together\n";
                    return 2;
                }
                return atperson::cli::run_jetstream_archive(
                    std::cout, resource_status, atperson::cli::data_dir(), graph, path,
                    atperson::cli::ledger_path(), atperson::cli::jetstream_state_path(),
                    after, before, span, collections_file, dids_file, print_stats);
            }
            int max_events = 0;
            int max_ms = 0;
            int positional = 0;
            std::filesystem::path collections_file =
                atperson::cli::jetstream_collections_path();
            std::filesystem::path dids_file =
                atperson::cli::jetstream_dids_path();
            std::filesystem::path kinds_file =
                atperson::cli::jetstream_kinds_path();

            for (int i = 2; i < argc; ++i) {
                const std::string_view argument = argv[i];
                if (argument == "--collections") {
                    if (i + 1 >= argc) {
                        std::cerr << "jetstream: --collections requires a file path\n";
                        return 2;
                    }
                    collections_file = argv[++i];
                    continue;
                }
                if (argument == "--dids") {
                    if (i + 1 >= argc) {
                        std::cerr << "jetstream: --dids requires a file path\n";
                        return 2;
                    }
                    dids_file = argv[++i];
                    continue;
                }
                if (argument == "--kinds") {
                    if (i + 1 >= argc) {
                        std::cerr << "jetstream: --kinds requires a file path\n";
                        return 2;
                    }
                    kinds_file = argv[++i];
                    continue;
                }
                if (argument.starts_with("--")) {
                    std::cerr << "jetstream: unknown option " << argument << '\n';
                    return 2;
                }
                if (positional == 0) {
                    max_events = atperson::cli::parse_limit(argv[i], 0);
                } else if (positional == 1) {
                    max_ms = atperson::cli::parse_limit(argv[i], 0);
                } else {
                    std::cerr << "jetstream: too many positional arguments\n";
                    return 2;
                }
                ++positional;
            }

            return atperson::cli::run_jetstream(
                std::cout, resource_status, atperson::cli::data_dir(), graph, path,
                atperson::cli::ledger_path(), atperson::cli::jetstream_state_path(),
                collections_file, dids_file, kinds_file, max_events, max_ms,
                print_stats);
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "atperson: " << error.what() << '\n';
        return 1;
    }
}
