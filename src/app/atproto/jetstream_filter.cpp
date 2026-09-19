#include "jetstream_filter.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
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
    return {"app.bsky.feed.post"};
}

std::vector<std::string>
load_jetstream_collections(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "jetstream collections: could not open " + path.string());
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
                "jetstream collections: line " + std::to_string(line_number) +
                " contains whitespace inside a collection filter");
        }
        if (!seen.insert(value).second) {
            continue;
        }
        if (result.size() == kJetstreamCollectionFilterLimit) {
            throw std::runtime_error(
                "jetstream collections: more than " +
                std::to_string(kJetstreamCollectionFilterLimit) +
                " unique collection filters");
        }
        result.push_back(std::move(value));
    }
    if (input.bad()) {
        throw std::runtime_error(
            "jetstream collections: could not read " + path.string());
    }
    if (result.empty()) {
        throw std::runtime_error(
            "jetstream collections: filter file contains no collections");
    }
    return result;
}

} // namespace atperson
