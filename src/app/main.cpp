#include "atperson/graph.hpp"
#include "atproto_client.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

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
        throw std::runtime_error(std::string("missing required environment variable ") +
                                 name);
    }
    return value;
}

std::filesystem::path state_path() {
    return env_or("ATPERSON_STATE", ".atperson/model.bin");
}

atperson::LanguageGraph load_or_create(const std::filesystem::path &path) {
    if (std::filesystem::exists(path)) {
        return atperson::LanguageGraph::load(path);
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
              << "mean loss: " << std::fixed << std::setprecision(6)
              << stats.mean_loss << '\n';
}

void usage(std::ostream &out) {
    out << "usage:\n"
        << "  atperson stats\n"
        << "  atperson ingest <text> [source-id]\n"
        << "  atperson ingest-file <path> [source-id]\n"
        << "  atperson assoc <token> [limit]\n"
        << "  atperson sync [limit]\n\n"
        << "environment:\n"
        << "  ATPERSON_STATE         model snapshot path "
           "(default .atperson/model.bin)\n"
        << "  ATPERSON_SERVICE       PDS/service URL "
           "(default https://bsky.social)\n"
        << "  ATPERSON_IDENTIFIER    handle or email for sync\n"
        << "  ATPERSON_APP_PASSWORD  app password for sync\n";
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
            const std::string source =
                argc >= 4 ? argv[3] : "local:manual";
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
                throw std::runtime_error("could not open " +
                                         input_path.string());
            }
            const std::string text((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
            const std::string source =
                argc >= 4 ? argv[3] : "file:" + input_path.string();
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
                std::cout << association.token << '\t' << std::fixed
                          << std::setprecision(4) << association.score << '\t'
                          << association.observations << '\t'
                          << std::hex << association.last_source_hash << std::dec
                          << '\n';
            }
            return 0;
        }

        if (command == "sync") {
            const int limit = argc >= 3 ? parse_limit(argv[2], 50) : 50;
            atperson::AtprotoClient client(
                env_or("ATPERSON_SERVICE", "https://bsky.social"),
                required_env("ATPERSON_IDENTIFIER"),
                required_env("ATPERSON_APP_PASSWORD"));

            const auto observations = client.fetch_timeline(limit);
            std::unordered_set<std::string> seen;
            std::size_t learned = 0u;
            for (const auto &observation : observations) {
                if (!seen.insert(observation.source_uri).second) {
                    continue;
                }
                graph.observe(observation.text, observation.source_uri);
                learned++;
            }
            graph.save(path);
            std::cout << "learned from " << learned
                      << " public timeline posts\n";
            print_stats(graph);
            return 0;
        }

        usage(std::cerr);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "atperson: " << error.what() << '\n';
        return 1;
    }
}
