#ifndef ATPERSON_ACTION_HPP
#define ATPERSON_ACTION_HPP

#include <cstdint>
#include <string>

namespace atperson {

struct ActionCandidate {
    std::string token;
    float score{};
    float association_score{};
    float familiarity_score{};
    float support_score{};
    std::uint64_t supporting_observations{};
    std::uint32_t context_matches{};
};

} // namespace atperson

#endif
