#include "cli/thoughts.hpp"

#include "control/state.hpp"
#include "journal/store.hpp"
#include "reflect/config.hpp"
#include "reflect/pass.hpp"
#include "state/lock.hpp"
#include "state/time.hpp"
#include "thought/store.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace atperson {
namespace cli {
namespace {

inline constexpr std::size_t kThoughtsListDefaultLimit = 50u;
inline constexpr std::size_t kThoughtsListMaximumLimit = 1000u;

} // namespace

std::filesystem::path thoughts_path(const std::filesystem::path &data_dir) {
    return data_dir / "thoughts";
}

int run_thought_record(std::ostream &out, const std::filesystem::path &data_dir,
                       const std::vector<std::string> &text_parts, std::string_view now_rfc3339) {
    std::string text;
    for (const std::string &part : text_parts) {
        if (!text.empty()) {
            text += ' ';
        }
        text += part;
    }
    {
        std::size_t begin = text.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            std::cerr << "thought: nothing to record\n";
            return 2;
        }
        const std::size_t end = text.find_last_not_of(" \t");
        text = text.substr(begin, end - begin + 1u);
    }

    const StateLock writers_lock(data_dir, "thoughts-lock");
    Thought entry;
    entry.id = new_thought_id();
    entry.kind = "reflection";
    entry.text = text;
    entry.at = std::string(now_rfc3339);
    write_thought(thoughts_path(data_dir), entry);

    out << "recorded " << entry.id << " " << entry.kind << " " << entry.at << ' ' << entry.text
        << '\n';
    return 0;
}

int run_thoughts_list(std::ostream &out, const std::filesystem::path &data_dir,
                      const std::string_view *arguments, std::size_t argument_count) {
    std::size_t limit = kThoughtsListDefaultLimit;
    std::string since;
    std::string kind;

    for (std::size_t i = 0u; i < argument_count; ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--since") {
            if (++i >= argument_count) {
                std::cerr << "thoughts: --since requires an RFC 3339 timestamp\n";
                return 2;
            }
            since = std::string(arguments[i]);
            if (!parse_rfc3339_epoch(since).has_value()) {
                std::cerr << "thoughts: --since must be an RFC 3339 timestamp\n";
                return 2;
            }
            continue;
        }
        if (argument == "--kind") {
            if (++i >= argument_count) {
                std::cerr << "thoughts: --kind requires a thought kind\n";
                return 2;
            }
            kind = std::string(arguments[i]);
            if (!thought_kind_is_valid(kind)) {
                std::cerr << "thoughts: unknown kind '" << kind << "'\n";
                return 2;
            }
            continue;
        }
        if (argument.starts_with("--")) {
            std::cerr << "thoughts: unexpected argument " << argument << '\n';
            return 2;
        }
        const std::string owned(argument);
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(owned.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            std::cerr << "thoughts: expected a numeric limit, got " << argument << '\n';
            return 2;
        }
        limit = static_cast<std::size_t>(
            std::min<unsigned long>(parsed, kThoughtsListMaximumLimit));
    }

    const ThoughtContents contents = load_thoughts(thoughts_path(data_dir));
    const std::optional<std::uint64_t> since_epoch =
        since.empty() ? std::nullopt : parse_rfc3339_epoch(since);

    std::size_t found = 0u;
    for (auto it = contents.thoughts.rbegin();
         it != contents.thoughts.rend() && found < limit; ++it) {
        const Thought &entry = *it;
        if (!kind.empty() && entry.kind != kind) {
            continue;
        }
        if (since_epoch.has_value()) {
            const std::optional<std::uint64_t> at = parse_rfc3339_epoch(entry.at);
            if (!at.has_value() || *at < *since_epoch) {
                continue;
            }
        }
        out << entry.id << ' ' << entry.kind << ' ' << entry.at << ' ' << entry.text << '\n';
        ++found;
    }
    return 0;
}

int run_reflect_command(std::ostream &out, const std::filesystem::path &data_dir,
                        const std::filesystem::path &journal_path, const LanguageGraph &graph,
                        std::int64_t now_unix, std::string_view now_rfc3339) {
    const ReflectionConfig config = reflection_config_from_environment();
    const StateLock writers_lock(data_dir, "thoughts-lock");
    const JournalContents journal = load_journal(journal_path);

    const ReflectionReport report =
        run_reflection_pass(thoughts_path(data_dir), journal, graph, config,
                            static_cast<std::uint64_t>(now_unix));

    out << "reflection: wrote " << report.thoughts_written << " thought(s) (max "
        << config.max_thoughts << "); window " << config.window_seconds << "s ending "
        << now_rfc3339 << ": " << report.valence_updates_in_window
        << " valence update(s) across " << report.valence_tokens_in_window << " token(s), "
        << report.episodes_in_window << " episode(s) from " << report.authors_in_window
        << " author(s), " << report.events_in_window << " linked event(s), "
        << report.resolutions_in_window << " resolution(s)\n";
    for (const ReflectionResult &result : report.written) {
        out << "  wrote " << result.thought.id << ' ' << result.thought.kind << " ("
            << result.from << ") " << result.thought.text << '\n';
    }
    return 0;
}

} // namespace cli
} // namespace atperson