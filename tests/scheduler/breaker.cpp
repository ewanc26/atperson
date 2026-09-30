/* Circuit breaker state machine and its persistence. Pure, injected clock. */

#include "scheduler/breaker.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

constexpr std::int64_t kT0 = 1789639200;

BreakerConfig config(std::uint32_t threshold = 3, std::int64_t base = 900,
                     std::int64_t max = 7200, std::uint32_t limit = 3) {
    BreakerConfig c;
    c.failure_threshold = threshold;
    c.base_cooldown_seconds = base;
    c.max_cooldown_seconds = max;
    c.proposal_failure_limit = limit;
    return c;
}

void test_opens_at_the_threshold_and_not_before() {
    BreakerState state;
    const BreakerConfig c = config(3);
    assert(breaker_gate(state, kT0) == BreakerGate::Closed);
    assert(!record_failure(state, c, "a", "boom", kT0).tripped);
    assert(!record_failure(state, c, "a", "boom", kT0 + 1).tripped);
    assert(breaker_gate(state, kT0 + 1) == BreakerGate::Closed);
    const FailureEffect effect = record_failure(state, c, "b", "boom 3", kT0 + 2);
    assert(effect.tripped);
    assert(state.trips == 1u && state.cooldown_seconds == 900);
    assert(state.open_until == kT0 + 2 + 900);
    assert(state.last_failure_detail == "boom 3");
    assert(breaker_gate(state, kT0 + 2) == BreakerGate::Open);
    assert(breaker_gate(state, kT0 + 2 + 899) == BreakerGate::Open);
    /* The cool-down elapsing does not close it: it becomes half-open. */
    assert(breaker_gate(state, kT0 + 2 + 900) == BreakerGate::HalfOpen);
}

void test_half_open_success_closes_and_resets() {
    BreakerState state;
    const BreakerConfig c = config(1);
    (void)record_failure(state, c, "a", "x", kT0);
    assert(breaker_gate(state, kT0 + 900) == BreakerGate::HalfOpen);
    record_success(state, "a");
    assert(breaker_gate(state, kT0 + 900) == BreakerGate::Closed);
    assert(state.consecutive_failures == 0u && state.cooldown_seconds == 0);
    /* A new trip starts again from the base cool-down. */
    (void)record_failure(state, c, "a", "x", kT0 + 5000);
    assert(state.cooldown_seconds == 900);
}

void test_half_open_failure_reopens_with_a_longer_cooldown_up_to_the_cap() {
    BreakerState state;
    const BreakerConfig c = config(1, 900, 3000);
    std::int64_t now = kT0;
    (void)record_failure(state, c, "a", "x", now);
    assert(state.cooldown_seconds == 900);
    for (const std::int64_t expected : {1800, 3000, 3000}) {
        now = state.open_until; /* the cool-down elapsed: half-open */
        assert(breaker_gate(state, now) == BreakerGate::HalfOpen);
        const FailureEffect effect = record_failure(state, c, "a", "x", now);
        assert(effect.tripped); /* one failed probe re-opens it at once */
        assert(state.cooldown_seconds == expected);
    }
    assert(state.trips == 4u);
}

void test_proposal_quarantine_after_the_limit() {
    BreakerState state;
    const BreakerConfig c = config(100, 900, 7200, 3);
    assert(!record_failure(state, c, "poison", "x", kT0).quarantine);
    assert(!record_failure(state, c, "poison", "x", kT0).quarantine);
    /* Failures of a different proposal are counted separately. */
    assert(!record_failure(state, c, "other", "x", kT0).quarantine);
    assert(record_failure(state, c, "poison", "x", kT0).quarantine);
    assert(state.proposal_failures.count("poison") == 0u); /* forgotten once set aside */
    assert(state.proposal_failures.at("other") == 1u);

    /* A success forgets a proposal's earlier failures. */
    (void)record_failure(state, c, "flaky", "x", kT0);
    record_success(state, "flaky");
    assert(state.proposal_failures.count("flaky") == 0u);
}

void test_retain_keeps_the_map_bounded() {
    BreakerState state;
    const BreakerConfig c = config(100);
    for (const char *digest : {"a", "b", "c"}) {
        (void)record_failure(state, c, digest, "x", kT0);
    }
    retain_proposals(state, {"b"});
    assert(state.proposal_failures.size() == 1u && state.proposal_failures.count("b") == 1u);
}

void test_persistence_round_trip_and_corruption() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("atperson-breaker-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "breaker.json";

    /* Missing file: closed, no history. */
    assert(breaker_gate(load_breaker_state(path), kT0) == BreakerGate::Closed);

    BreakerState state;
    const BreakerConfig c = config(1);
    (void)record_failure(state, c, "digest-1", "write_failed: network down", kT0);
    save_breaker_state(state, path);
    const BreakerState loaded = load_breaker_state(path);
    assert(loaded.open_until == state.open_until && loaded.trips == 1u);
    assert(loaded.cooldown_seconds == 900 && loaded.consecutive_failures == 1u);
    assert(loaded.last_failure_detail == "write_failed: network down");
    assert(loaded.proposal_failures.at("digest-1") == 1u);

    /* Corruption is reported, never silently reset to "closed". */
    for (const char *bad : {"", "{", "[]", "{\"version\":2}", "{\"version\":1}",
                            "{\"version\":1,\"consecutive_failures\":-1,\"open_until\":0,"
                            "\"cooldown_seconds\":0,\"trips\":0,\"last_failure_at\":0,"
                            "\"proposal_failures\":{}}"}) {
        std::ofstream(path, std::ios::trunc) << bad;
        bool threw = false;
        try {
            (void)load_breaker_state(path);
        } catch (const BreakerError &) {
            threw = true;
        }
        assert(threw);
    }
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_opens_at_the_threshold_and_not_before();
    test_half_open_success_closes_and_resets();
    test_half_open_failure_reopens_with_a_longer_cooldown_up_to_the_cap();
    test_proposal_quarantine_after_the_limit();
    test_retain_keeps_the_map_bounded();
    test_persistence_round_trip_and_corruption();
    std::puts("breaker tests passed");
    return 0;
}
