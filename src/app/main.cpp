#include "atperson/bootstrap.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "atproto_client.hpp"
#include "ingestion_state.hpp"
#include "state_lock.hpp"
#include "sync_engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

std::string env_or(const char *name, std::string fallback = {}) {
    if (const char *value = std::getenv(name); value && value[0] != '\0') {
        return value;
    }
    return fallback;
}

std::string required_env(const char *name) {
    const std::string value = env_or(name);
    if (value.empty()) {
        throw std::runtime_error(std::string("missing required environment variable ") + name);
    }
    return value;
}

/* Default data directory: $ATPERSON_HOME, else ~/.ewanc26/atperson. Falls
 * back to ".atperson" relative to the working directory only when no home
 * directory can be determined. */
std::filesystem::path data_dir() {
    if (const std::string home = env_or("ATPERSON_HOME"); !home.empty()) {
        return home;
    }
    const char *home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".ewanc26" / "atperson";
    }
    return ".atperson";
}

std::filesystem::path state_path() {
    return env_or("ATPERSON_STATE", (data_dir() / "model.bin").string());
}

std::filesystem::path ledger_path() {
    return env_or("ATPERSON_LEDGER", (data_dir() / "ledger.bin").string());
}

std::filesystem::path ingestion_state_path() {
    return env_or("ATPERSON_INGESTION_STATE",
                  (data_dir() / "ingestion-state.json").string());
}

atperson::LanguageGraph load_or_create(const std::filesystem::path &path) {
    if (std::filesystem::exists(path)) {
        return atperson::LanguageGraph::load(path);
    }
    /* First run: make sure the data directory exists before anything saves. */
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    return atperson::LanguageGraph();
}

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

void usage(std::ostream &out) {
    out << "usage:\n"
        << "  atperson stats\n"
        << "  atperson ingest <text> [source-id]\n"
        << "  atperson ingest-file <path> [source-id]\n"
        << "  atperson assoc <token> [limit]\n"
        << "  atperson candidates <context> [limit]\n"
        << "  atperson familiarity <token>\n"
        << "  atperson recall <query> [limit]\n"
        << "  atperson sync [max-pages]\n"
        << "  atperson cursor [status|reset]\n\n"
        << "environment:\n"
        << "  ATPERSON_STATE            model snapshot path "
           "(default ~/.ewanc26/atperson/model.bin)\n"
        << "  ATPERSON_LEDGER           observation ledger path "
           "(default ~/.ewanc26/atperson/ledger.bin)\n"
        << "  ATPERSON_INGESTION_STATE  ingestion cursor path "
           "(default ~/.ewanc26/atperson/ingestion-state.json)\n"
        << "  ATPERSON_HOME             data directory override "
           "(default ~/.ewanc26/atperson)\n"
        << "  ATPERSON_SYNC_PAGE_SIZE   items per timeline page (default 50)\n"
        << "  ATPERSON_SERVICE       PDS/service URL "
           "(default https://bsky.social)\n"
        << "  ATPERSON_IDENTIFIER    handle or email for sync\n"
        << "  ATPERSON_APP_PASSWORD  app password for sync\n\n"
        << "mutating commands take an exclusive lock on the data directory;\n"
        << "read-only commands run without it and see state as of their read\n";
}

int parse_limit(const char *value, int fallback) {
    if (!value) {
        return fallback;
    }
    const int parsed = std::stoi(value);
    if (parsed <= 0) {
        throw std::runtime_error("limit must be positive");
    }
    return parsed;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }

        /* First-run bootstrap (C23 core): create the data directory and a
         * .env template when either is missing. Idempotent; never touches
         * existing files. */
        char home_buffer[4096];
        if (atp_default_home_directory(home_buffer, sizeof(home_buffer), nullptr) !=
            nullptr) {
            char notice[ATP_BOOTSTRAP_NOTICE_BYTES];
            if (atp_bootstrap_home(home_buffer, notice, sizeof(notice)) == ATP_OK &&
                notice[0] != '\0') {
                std::cerr << "atperson: " << notice << '\n';
            }
        }

        const std::string_view command = argv[1];
        const auto path = state_path();
        auto graph = load_or_create(path);

        if (command == "stats") {
            print_stats(graph);
            return 0;
        }

        if (command == "ingest") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            const atperson::StateLock writer_lock(data_dir());
            const std::string source = argc >= 4 ? argv[3] : "local:manual";
            graph.observe(argv[2], source);
            graph.save(path);
            print_stats(graph);
            return 0;
        }

        if (command == "ingest-file") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            const std::filesystem::path input_path = argv[2];
            std::ifstream input(input_path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("could not open " + input_path.string());
            }
            const atperson::StateLock writer_lock(data_dir());
            const std::string text((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
            const std::string source = argc >= 4 ? argv[3] : "file:" + input_path.string();
            graph.observe(text, source);
            graph.save(path);
            print_stats(graph);
            return 0;
        }
        if (command == "assoc") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            const int limit = argc >= 4 ? parse_limit(argv[3], 10) : 10;
            for (const auto &association :
                 graph.associations(argv[2], static_cast<std::size_t>(limit))) {
                std::cout << association.token << '\t' << std::fixed << std::setprecision(4)
                          << association.score << '\t' << association.observations << '\t'
                          << std::hex << association.last_source_hash << std::dec << '\n';
            }
            return 0;
        }

        if (command == "candidates") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            const int limit = argc >= 4 ? parse_limit(argv[3], 10) : 10;
            for (const auto &candidate :
                 graph.action_candidates(argv[2], static_cast<std::size_t>(limit))) {
                std::cout << candidate.token << '\t' << std::fixed << std::setprecision(4)
                          << candidate.score << '\t' << candidate.association_score << '\t'
                          << candidate.familiarity_score << '\t' << candidate.support_score << '\t'
                          << candidate.supporting_observations << '\t' << candidate.context_matches
                          << '\n';
            }
            return 0;
        }

        if (command == "familiarity") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            std::cout << std::fixed << std::setprecision(4) << graph.familiarity(argv[2]) << '\n';
            return 0;
        }
        if (command == "recall") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            const int limit = argc >= 4 ? parse_limit(argv[3], 10) : 10;
            const std::uint64_t at_epoch = static_cast<std::uint64_t>(std::time(nullptr));
            for (const auto &episode :
                 graph.recall(argv[2], at_epoch, static_cast<std::size_t>(limit))) {
                std::cout << "ledger " << episode.ledger_id << '\t' << "at " << episode.observed_at
                          << '\t' << "recall " << episode.recall_count << '\t' << episode.source_id
                          << '\t';
                for (std::uint32_t t = 0u; t < episode.token_count; ++t) {
                    std::cout << '<' << episode.summary[t].node_index << ':' << std::fixed
                              << std::setprecision(1) << episode.summary[t].weight << "> ";
                }
                std::cout << '\n';
            }
            return 0;
        }

        if (command == "sync") {
            const atperson::StateLock writer_lock(data_dir());
            const int max_pages = argc >= 3 ? parse_limit(argv[2], 1) : 1;
            atperson::AtprotoClient client(env_or("ATPERSON_SERVICE", "https://bsky.social"),
                                           required_env("ATPERSON_IDENTIFIER"),
                                           required_env("ATPERSON_APP_PASSWORD"));

            const std::filesystem::path ledger_dir = ledger_path();
            if (const auto parent = ledger_dir.parent_path(); !parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            atperson::Ledger ledger(ledger_dir);

            const std::filesystem::path state_file = ingestion_state_path();
            auto ingestion = atperson::load_ingestion_state(
                state_file, env_or("ATPERSON_SERVICE", "https://bsky.social"),
                client.account_did());

            atperson::SyncLimits limits;
            limits.page_size = std::stoi(env_or("ATPERSON_SYNC_PAGE_SIZE", "50"));
            limits.max_pages = max_pages;

            const auto fetch_page = [&client, &limits](
                                        const std::optional<std::string> &cursor) {
                try {
                    return client.fetch_timeline_page(cursor, limits.page_size);
                } catch (const atperson::TimelineHttpError &) {
                    if (cursor) {
                        // The service rejected the persisted cursor. It is a
                        // disposable fetching checkpoint, not learned state:
                        // reset to the head and let ledger dedup suppress
                        // anything already committed.
                        std::cerr << "atperson: saved cursor rejected by the service; "
                                     "resetting to the timeline head\n";
                        return client.fetch_timeline_page(std::nullopt, limits.page_size);
                    }
                    throw;
                }
            };

            const auto result = atperson::run_sync(graph, ledger, ingestion, fetch_page,
                                                   limits);

            graph.save(path);
            ingestion.checkpoint.generation++;
            atperson::save_ingestion_state(ingestion, state_file);
            std::cout << "completed " << result.pages_completed << " page(s), "
                      << result.observations_seen << " observation(s)"
                      << (result.exhausted ? ", timeline exhausted" : ", catch-up pending")
                      << "; learned from " << result.learned << " (skipped "
                      << result.skipped << ", duplicate " << result.duplicates
                      << ") public timeline posts\n";
            print_stats(graph);
            return 0;
        }

        if (command == "cursor") {
            const std::string sub = argc >= 3 ? argv[2] : "status";
            if (sub != "status" && sub != "reset") {
                usage(std::cerr);
                return 2;
            }
            const std::filesystem::path state_file = ingestion_state_path();
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            /* Status and reset both bind to the current session so a stale
             * cursor from another account is neither shown nor reused. */
            atperson::AtprotoClient client(service, required_env("ATPERSON_IDENTIFIER"),
                                           required_env("ATPERSON_APP_PASSWORD"));

            auto state = atperson::load_ingestion_state(state_file, service,
                                                        client.account_did());
            if (sub == "reset") {
                const atperson::StateLock writer_lock(data_dir());
                atperson::reset_ingestion_state(state);
                state.checkpoint.generation++;
                atperson::save_ingestion_state(state, state_file);
                std::cout << "ingestion cursor reset; next sync starts at the timeline head\n";
                return 0;
            }

            std::cout << "source: " << state.source.kind << " " << state.source.service
                      << " " << state.source.account_did << " " << state.source.endpoint
                      << '\n'
                      << "catch-up: "
                      << (state.catchup.active ? "active" : "inactive") << '\n'
                      << "checkpoint: generation " << state.checkpoint.generation
                      << ", pages " << state.checkpoint.pages_completed << ", observations "
                      << state.checkpoint.observations_seen << '\n';
            return 0;
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "atperson: " << error.what() << '\n';
        return 1;
    }
}
