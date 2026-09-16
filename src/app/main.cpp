#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "atproto_client.hpp"

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

std::filesystem::path state_path() {
    return env_or("ATPERSON_STATE", ".atperson/model.bin");
}

std::filesystem::path ledger_path() {
    return env_or("ATPERSON_LEDGER", ".atperson/ledger.bin");
}

/* Parse an RFC 3339 timestamp ("YYYY-MM-DDTHH:MM:SS[.frac][Z|±HH:MM]") to Unix
 * epoch seconds. Returns nullopt for anything that is not a full, valid
 * instant so callers can fall back to an explicit "unknown" value.
 *
 * Parsed by hand rather than std::chrono::parse, which is not yet implemented
 * by the platform's standard library. The day-of-month conversion uses the
 * days-from-civil algorithm so it stays pure C++ with no time-zone
 * dependencies. */
std::optional<std::uint64_t> parse_rfc3339_epoch(std::string_view value) {
    if (value.size() < 19u) {
        return std::nullopt;
    }
    for (std::size_t i = 0u; i < 19u; ++i) {
        const bool digit = value[i] >= '0' && value[i] <= '9';
        const bool separator = i == 4u || i == 7u || i == 13u || i == 16u;
        if (!digit && !(separator && value[i] == '-') && !(i == 10u && value[i] == 'T')) {
            return std::nullopt;
        }
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(std::string(value.substr(0u, 19u)).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year,
                    &month, &day, &hour, &minute, &second) != 6) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }

    std::size_t position = 19u;
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            ++position;
        }
    }

    long offset_seconds = 0;
    if (position < value.size()) {
        const char zone = value[position];
        if (zone == 'Z' || zone == 'z') {
            ++position;
        } else if (zone == '+' || zone == '-') {
            if (position + 6u > value.size() || value[position + 3u] != ':') {
                return std::nullopt;
            }
            long hours = 0;
            long minutes = 0;
            try {
                hours = std::stol(std::string(value.substr(position + 1u, 2u)));
                minutes = std::stol(std::string(value.substr(position + 4u, 2u)));
            } catch (const std::exception &) {
                return std::nullopt;
            }
            if (hours > 23 || minutes > 59) {
                return std::nullopt;
            }
            offset_seconds = hours * 3600 + minutes * 60;
            if (zone == '-') {
                offset_seconds = -offset_seconds;
            }
            position += 6u;
        } else {
            return std::nullopt;
        }
    }
    if (position != value.size()) {
        return std::nullopt;
    }

    const std::int64_t adjusted_month =
        month > 2 ? static_cast<std::int64_t>(month) : static_cast<std::int64_t>(month + 12);
    const std::int64_t adjusted_year = year - (month > 2 ? 0 : 1);
    const std::int64_t era = adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
    const std::int64_t year_of_era = adjusted_year - era * 400;
    const std::int64_t day_of_year =
        (153 * (adjusted_month > 2 ? adjusted_month - 3 : adjusted_month + 9) + 2) / 5 + day - 1;
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const std::int64_t epoch_days = era * 146097 + day_of_era - 719468;
    const std::int64_t local_seconds = epoch_days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
                                       static_cast<std::int64_t>(minute) * 60 + second;
    const std::int64_t epoch = local_seconds - offset_seconds;
    if (epoch < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(epoch);
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
        << "  atperson familiarity <token>\n"
        << "  atperson recall <query> [limit]\n"
        << "  atperson sync [limit]\n\n"
        << "environment:\n"
        << "  ATPERSON_STATE         model snapshot path "
           "(default .atperson/model.bin)\n"
        << "  ATPERSON_LEDGER        observation ledger path "
           "(default .atperson/ledger.bin)\n"
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
            const int limit = argc >= 3 ? parse_limit(argv[2], 50) : 50;
            atperson::AtprotoClient client(env_or("ATPERSON_SERVICE", "https://bsky.social"),
                                           required_env("ATPERSON_IDENTIFIER"),
                                           required_env("ATPERSON_APP_PASSWORD"));

            const std::filesystem::path ledger_dir = ledger_path();
            if (const auto parent = ledger_dir.parent_path(); !parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            atperson::Ledger ledger(ledger_dir);

            const auto observations = client.fetch_timeline(limit);
            std::size_t learned = 0u;
            std::size_t remembered = 0u;
            std::size_t skipped = 0u;
            std::size_t duplicates = 0u;
            for (const auto &observation : observations) {
                const auto source_id = static_cast<std::string>(observation.source_uri);
                const std::uint64_t digest = atperson::Ledger::digest(observation.text);
                const std::uint64_t observed_at =
                    parse_rfc3339_epoch(observation.created_at).value_or(0u);

                // Record the post durably before training so a restarted
                // process can never re-train on it: on restart the ledger is
                // the authority for what has already been committed.
                std::uint64_t id = 0u;
                const auto result =
                    ledger.append(source_id, observation.author_did, observed_at, digest,
                                  ATPERSON_SCHEMA_VERSION, ATP_LEDGER_OUTCOME_PENDING, &id);
                if (result == atperson::LedgerResult::ExistsCommitted) {
                    duplicates++;
                    continue;
                }

                const bool trainable = !observation.text.empty();
                const auto outcome =
                    trainable ? ATP_LEDGER_OUTCOME_LEARNED : ATP_LEDGER_OUTCOME_SKIPPED;
                if (trainable) {
                    if (graph.remember(observation.text, source_id, observation.author_did,
                                       observed_at, digest, ATPERSON_SCHEMA_VERSION, id)) {
                        remembered++;
                    }
                    learned++;
                } else {
                    skipped++;
                }
                ledger.set_outcome(id, outcome);

                atp_ledger_entry entry = {};
                entry.id = id;
                entry.observed_at = observed_at;
                entry.content_digest = digest;
                entry.schema_version = ATPERSON_SCHEMA_VERSION;
                entry.outcome = outcome;
                std::memcpy(entry.source_id, source_id.data(), source_id.size());
                entry.source_id[source_id.size()] = '\0';
                const std::size_t author_len =
                    observation.author_did.size() < sizeof(entry.author_did) - 1u
                        ? observation.author_did.size()
                        : sizeof(entry.author_did) - 1u;
                std::memcpy(entry.author_did, observation.author_did.data(), author_len);
                entry.author_did[author_len] = '\0';
                graph.record_ledger_entry(entry);
            }

            graph.save(path);
            std::cout << "learned from " << learned << " (remembered " << remembered << ", skipped "
                      << skipped << ", duplicate " << duplicates << ") public timeline posts\n";
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
