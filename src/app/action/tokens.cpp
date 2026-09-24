#include "tokens.hpp"

#include "atperson/core.h"
#include "atperson/tokenize.h"

#include <unordered_set>

namespace atperson {
namespace action {
namespace {

struct Collector {
    std::vector<std::string> *out;
    std::unordered_set<std::string> *seen;
    std::size_t max_tokens{};
};

/* Emit one token; returns false to stop the scan as soon as the bound is
 * reached, so a bounded collection never scans past what it keeps. */
bool emit_token(void *userdata, const char *token) {
    auto *collector = static_cast<Collector *>(userdata);
    if (collector->seen->insert(token).second) {
        collector->out->emplace_back(token);
    }
    return collector->out->size() < collector->max_tokens;
}

std::vector<std::string> collect(std::string_view text, std::size_t max_tokens) {
    std::vector<std::string> tokens;
    if (text.empty() || max_tokens == 0u) {
        return tokens;
    }
    std::unordered_set<std::string> seen;
    if (max_tokens != std::string::npos) {
        tokens.reserve(max_tokens);
        seen.reserve(max_tokens);
    }
    Collector collector{&tokens, &seen, max_tokens};
    const std::string owned(text);
    atp_tokenize(owned.c_str(), ATPERSON_SCHEMA_VERSION, emit_token, &collector);
    return tokens;
}

} // namespace

std::vector<std::string> distinct_tokens(std::string_view text) {
    return collect(text, std::string::npos);
}

std::vector<std::string> distinct_tokens(std::string_view text, std::size_t max_tokens) {
    return collect(text, max_tokens);
}

} // namespace action
} // namespace atperson