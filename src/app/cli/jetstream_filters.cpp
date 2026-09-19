#include "jetstream_filters.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace atperson::cli {

std::vector<std::string> load_jetstream_collections(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("could not open Jetstream collections file " + path.string());
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        std::size_t first = 0;
        while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
        line.erase(0, first);
        if (line.empty()) continue;
        if (std::any_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }))
            throw std::runtime_error("Jetstream collection contains embedded whitespace");
        if (std::find(result.begin(), result.end(), line) == result.end()) result.push_back(line);
        if (result.size() > 100u) throw std::runtime_error("Jetstream collection filter exceeds 100 entries");
    }
    if (result.empty()) throw std::runtime_error("Jetstream collection filter is empty");
    return result;
}

std::vector<std::string> load_jetstream_dids(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("could not open Jetstream DIDs file " + path.string());
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) {
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        std::size_t first = 0;
        while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
        line.erase(0, first);
        if (line.empty()) continue;
        if (std::any_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }) ||
            line.rfind("did:", 0) != 0)
            throw std::runtime_error("Jetstream DID filter must contain whitespace-free did: identifiers");
        if (std::find(result.begin(), result.end(), line) == result.end()) result.push_back(line);
        if (result.size() > 10000u) throw std::runtime_error("Jetstream DID filter exceeds 10000 entries");
    }
    if (result.empty()) throw std::runtime_error("Jetstream DID filter is empty");
    return result;
}

} // namespace atperson::cli
