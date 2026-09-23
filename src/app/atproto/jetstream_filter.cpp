#include "jetstream_filter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace atperson {
namespace {

std::string trim_ascii(std::string value) {
    const auto whitespace = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool has_ascii_whitespace(const std::string &value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    });
}

} // namespace

std::vector<std::string> default_jetstream_collections() {
    /* No collection predicate asks Jetstream for every public collection. */
    return {};
}

namespace {

std::vector<std::string> load_filter_file(
    const std::filesystem::path &path, std::size_t limit,
    const char *label, bool require_did) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            std::string("jetstream ") + label + ": could not open " +
            path.string());
    }

    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    std::string line;
    std::size_t line_number = 0u;
    while (std::getline(input, line)) {
        ++line_number;
        std::string value = trim_ascii(std::move(line));
        if (value.empty() || value.front() == '#') {
            continue;
        }
        if (has_ascii_whitespace(value)) {
            throw std::runtime_error(
                std::string("jetstream ") + label + ": line " +
                std::to_string(line_number) +
                " contains whitespace inside a filter");
        }
        if (require_did && value.rfind("did:", 0u) != 0u) {
            throw std::runtime_error(
                std::string("jetstream ") + label + ": line " +
                std::to_string(line_number) + " is not a DID");
        }
        if (!seen.insert(value).second) {
            continue;
        }
        if (result.size() == limit) {
            throw std::runtime_error(
                std::string("jetstream ") + label + ": more than " +
                std::to_string(limit) + " unique filters");
        }
        result.push_back(std::move(value));
    }
    if (input.bad()) {
        throw std::runtime_error(
            std::string("jetstream ") + label + ": could not read " +
            path.string());
    }
    if (result.empty()) {
        throw std::runtime_error(
            std::string("jetstream ") + label +
            ": filter file contains no entries");
    }
    return result;
}

} // namespace

std::vector<std::string>
load_jetstream_collections(const std::filesystem::path &path) {
    return load_filter_file(path, kJetstreamCollectionFilterLimit,
                            "collections", false);
}

std::vector<std::string>
load_jetstream_dids(const std::filesystem::path &path) {
    return load_filter_file(path, kJetstreamDidFilterLimit, "DIDs", true);
}

std::vector<std::string>
load_jetstream_kinds(const std::filesystem::path &path) {
    static constexpr std::string_view kValidKinds[] = {
        "commit", "identity", "account", "sync"};
    std::vector<std::string> result =
        load_filter_file(path, kJetstreamKindFilterLimit, "kinds", false);
    for (const std::string &kind : result) {
        const bool valid = std::any_of(
            std::begin(kValidKinds), std::end(kValidKinds),
            [&kind](std::string_view candidate) { return kind == candidate; });
        if (!valid) {
            throw std::runtime_error(
                "jetstream kinds: unknown event kind '" + kind +
                "' (expected one of: commit, identity, account, sync)");
        }
    }
    return result;
}

} // namespace atperson
