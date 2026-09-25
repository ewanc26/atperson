/* Supervisor health surface (#143): heartbeat round-trip, atomic save,
 * staleness verdicts and fail-closed unreadable cases. Offline, no
 * network, real filesystem. */
#include "autonomy/heartbeat.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

using atperson::AutonomyHealth;
using atperson::AutonomyHeartbeat;
using atperson::autonomy_health;
using atperson::autonomy_health_name;
using atperson::load_autonomy_heartbeat;
using atperson::parse_autonomy_heartbeat;
using atperson::save_autonomy_heartbeat;
using atperson::serialise_autonomy_heartbeat;

constexpr std::int64_t NOW = 1'700'000'000;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-heartbeat-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

AutonomyHeartbeat sample_beat() {
    AutonomyHeartbeat beat;
    beat.run_id = "2026-09-24T00:00:00Z";
    beat.cycle = 42;
    beat.beat_at = "2026-09-24T00:10:00Z";
    beat.phase = "learning";
    return beat;
}

void test_round_trip() {
    const AutonomyHeartbeat beat = sample_beat();
    const std::string json = serialise_autonomy_heartbeat(beat);
    const AutonomyHeartbeat parsed = parse_autonomy_heartbeat(json);
    assert(parsed.version == beat.version);
    assert(parsed.run_id == beat.run_id);
    assert(parsed.cycle == beat.cycle);
    assert(parsed.beat_at == beat.beat_at);
    assert(parsed.phase == beat.phase);
    std::puts("heartbeat round trip: ok");
}

void test_save_load() {
    const std::filesystem::path root = scratch_dir("save");
    const std::filesystem::path file = root / "beat.json";
    save_autonomy_heartbeat(sample_beat(), file);
    const auto loaded = load_autonomy_heartbeat(file);
    assert(loaded.has_value());
    assert(loaded->cycle == 42u);
    assert(loaded->phase == "learning");
    /* No temp file left behind. */
    assert(!std::filesystem::exists(root / "beat.json.tmp"));
    std::puts("heartbeat save/load atomic: ok");
}

void test_missing_is_unreadable() {
    const std::filesystem::path root = scratch_dir("missing");
    const auto report = autonomy_health(root / "beat.json", NOW, 900);
    assert(report.health == AutonomyHealth::Unreadable);
    assert(!report.heartbeat.has_value());
    std::puts("missing heartbeat is unreadable (fail-closed): ok");
}

void test_corrupt_is_unreadable() {
    const std::filesystem::path root = scratch_dir("corrupt");
    const std::filesystem::path file = root / "beat.json";
    std::ofstream(file) << "{not json";
    const auto report = autonomy_health(file, NOW, 900);
    assert(report.health == AutonomyHealth::Unreadable);
    std::puts("corrupt heartbeat is unreadable: ok");
}

void test_fresh_is_healthy() {
    const std::filesystem::path root = scratch_dir("fresh");
    const std::filesystem::path file = root / "beat.json";
    AutonomyHeartbeat beat = sample_beat();
    /* RFC 3339 for NOW - 60s: fixed offset arithmetic on a known epoch. */
    beat.beat_at = "2023-11-14T22:13:20Z"; /* NOW - 60 */
    save_autonomy_heartbeat(beat, file);
    const auto report = autonomy_health(file, NOW, 900);
    assert(report.health == AutonomyHealth::Healthy);
    assert(report.heartbeat.has_value());
    std::puts("fresh heartbeat is healthy: ok");
}

void test_old_is_stale() {
    const std::filesystem::path root = scratch_dir("stale");
    const std::filesystem::path file = root / "beat.json";
    AutonomyHeartbeat beat = sample_beat();
    beat.beat_at = "2023-11-14T21:00:00Z"; /* NOW - 4600 > 900 */
    save_autonomy_heartbeat(beat, file);
    const auto report = autonomy_health(file, NOW, 900);
    assert(report.health == AutonomyHealth::Stale);
    assert(report.heartbeat.has_value());
    std::puts("old heartbeat is stale: ok");
}

void test_bad_timestamp_is_unreadable() {
    const std::filesystem::path root = scratch_dir("badts");
    const std::filesystem::path file = root / "beat.json";
    AutonomyHeartbeat beat = sample_beat();
    beat.beat_at = "yesterday";
    save_autonomy_heartbeat(beat, file);
    const auto report = autonomy_health(file, NOW, 900);
    assert(report.health == AutonomyHealth::Unreadable);
    std::puts("non-RFC-3339 timestamp is unreadable: ok");
}

} // namespace

int main() {
    test_round_trip();
    test_save_load();
    test_missing_is_unreadable();
    test_corrupt_is_unreadable();
    test_fresh_is_healthy();
    test_old_is_stale();
    test_bad_timestamp_is_unreadable();
    return 0;
}
