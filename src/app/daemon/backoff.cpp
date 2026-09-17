#include "backoff.hpp"

#include <algorithm>
#include <cstdint>

namespace atperson {
namespace {

constexpr std::uint64_t kDefaultSeed = 0x9e3779b97f4a7c15ull;

/* splitmix64: a small deterministic PRNG for jitter. No standard-library
 * distribution state, so the sequence is identical across platforms. */
std::uint64_t splitmix64(std::uint64_t &state) noexcept {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31u);
}

} // namespace

Backoff::Backoff(const BackoffConfig &config, std::uint64_t seed)
    : config_(config), rng_(seed == 0u ? kDefaultSeed : seed) {}

std::chrono::milliseconds Backoff::next() {
    if (failures_ == 0u) {
        current_ = config_.initial;
    } else {
        const double scaled = static_cast<double>(current_.count()) * config_.factor;
        const double capped = std::min(scaled, static_cast<double>(config_.maximum.count()));
        current_ = std::chrono::milliseconds(static_cast<long long>(capped));
    }
    ++failures_;

    std::chrono::milliseconds delay = current_;
    if (config_.jitter > 0.0) {
        const double unit = static_cast<double>(splitmix64(rng_)) / static_cast<double>(UINT64_MAX);
        const double extra = unit * config_.jitter * static_cast<double>(delay.count());
        const double jittered = static_cast<double>(delay.count()) + extra;
        const double bounded = std::min(jittered, static_cast<double>(config_.maximum.count()));
        delay = std::chrono::milliseconds(static_cast<long long>(bounded));
    }
    return delay;
}

void Backoff::reset() noexcept {
    current_ = std::chrono::milliseconds{};
    failures_ = 0u;
}

} // namespace atperson
