/* Operator control state (#22): fail-closed defaults, the outbound write
 * gate, approval binding, and atomic persistence round-trips. Offline. */
#include "control/state.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-control-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

/* Every gate open: writes enabled, dry-run off, approval satisfied. */
atperson::ControlState armed_state() {
    atperson::ControlState state;
    state.writes_enabled = true;
    state.dry_run = false;
    state.approval_required = true;
    state.approved_digests = {"0123456789abcdef"};
    return state;
}

void test_missing_file_is_fail_closed() {
    const auto dir = scratch_dir("missing");
    const auto path = dir / "control-state.json";

    const auto state = atperson::load_control_state(path);
    assert(!state.paused);
    assert(!state.writes_enabled);
    assert(state.dry_run);
    assert(state.approval_required);
    assert(state.approved_digests.empty());
    assert(!state.last_sync_at.has_value());
    assert(!state.shutdown_requested_at.has_value());

    bool refused = false;
    try {
        atperson::ensure_outbound_allowed(state, "0123456789abcdef");
    } catch (const atperson::ControlStateError &) {
        refused = true;
    }
    assert(refused);
    std::printf("ok missing file is fail-closed\n");
}

void test_gate_refuses_each_closed_switch() {
    /* Paused blocks even a fully-armed state. */
    {
        auto state = armed_state();
        state.paused = true;
        bool refused = false;
        try {
            atperson::ensure_outbound_allowed(state, "0123456789abcdef");
        } catch (const atperson::ControlStateError &) {
            refused = true;
        }
        assert(refused);
    }
    /* Write gate. */
    {
        auto state = armed_state();
        state.writes_enabled = false;
        bool refused = false;
        try {
            atperson::ensure_outbound_allowed(state, "0123456789abcdef");
        } catch (const atperson::ControlStateError &) {
            refused = true;
        }
        assert(refused);
    }
    /* Dry-run. */
    {
        auto state = armed_state();
        state.dry_run = true;
        bool refused = false;
        try {
            atperson::ensure_outbound_allowed(state, "0123456789abcdef");
        } catch (const atperson::ControlStateError &) {
            refused = true;
        }
        assert(refused);
    }
    /* Unapproved digest. */
    {
        auto state = armed_state();
        bool refused = false;
        try {
            atperson::ensure_outbound_allowed(state, "ffffffffffffffff");
        } catch (const atperson::ControlStateError &) {
            refused = true;
        }
        assert(refused);
    }
    /* Approval not required: digest irrelevant. */
    {
        auto state = armed_state();
        state.approval_required = false;
        atperson::ensure_outbound_allowed(state, "ffffffffffffffff");
    }
    /* Every gate open: allowed. */
    {
        const auto state = armed_state();
        atperson::ensure_outbound_allowed(state, "0123456789abcdef");
    }
    std::printf("ok gate refuses each closed switch\n");
}

void test_round_trip_preserves_every_field() {
    const auto dir = scratch_dir("roundtrip");
    const auto path = dir / "control-state.json";

    atperson::ControlState saved;
    saved.paused = true;
    saved.writes_enabled = true;
    saved.dry_run = false;
    saved.approval_required = true;
    saved.approved_digests = {"0123456789abcdef", "fedcba9876543210"};
    saved.last_sync_at = "2026-09-17T12:00:00Z";
    saved.shutdown_requested_at = "2026-09-17T13:00:00Z";
    atperson::save_control_state(saved, path);

    const auto loaded = atperson::load_control_state(path);
    assert(loaded.paused == saved.paused);
    assert(loaded.writes_enabled == saved.writes_enabled);
    assert(loaded.dry_run == saved.dry_run);
    assert(loaded.approval_required == saved.approval_required);
    assert(loaded.approved_digests == saved.approved_digests);
    assert(loaded.last_sync_at == saved.last_sync_at);
    assert(loaded.shutdown_requested_at == saved.shutdown_requested_at);

    /* Serialisation is deterministic: same state, same bytes. */
    assert(atperson::serialise_control_state(saved) ==
           atperson::serialise_control_state(loaded));

    /* The approved digest survives the round trip and still opens the gate. */
    atperson::ControlState armed = loaded;
    armed.paused = false;
    armed.dry_run = false;
    atperson::ensure_outbound_allowed(armed, "fedcba9876543210");
    std::printf("ok round trip preserves every field\n");
}

void test_corrupt_and_invalid_files_throw() {
    const auto dir = scratch_dir("corrupt");
    const auto path = dir / "control-state.json";

    write_file(path, "not json at all");
    bool threw = false;
    try {
        (void)atperson::load_control_state(path);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);

    /* Wrong format marker. */
    write_file(path, "{\"format\":\"something-else\",\"version\":1}");
    threw = false;
    try {
        (void)atperson::load_control_state(path);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);

    /* Unsupported version. */
    write_file(path, "{\"format\":\"atperson-control-state\",\"version\":2}");
    threw = false;
    try {
        (void)atperson::load_control_state(path);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);

    /* Non-boolean gate. */
    write_file(path,
               "{\"format\":\"atperson-control-state\",\"version\":1,\"paused\":\"yes\","
               "\"writes_enabled\":false,\"dry_run\":true,\"approval_required\":true,"
               "\"approved_digests\":[]}");
    threw = false;
    try {
        (void)atperson::load_control_state(path);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);

    /* Malformed digest shape is rejected, not silently dropped. */
    write_file(path,
               "{\"format\":\"atperson-control-state\",\"version\":1,\"paused\":false,"
               "\"writes_enabled\":false,\"dry_run\":true,\"approval_required\":true,"
               "\"approved_digests\":[\"NOT-A-DIGEST\"]}");
    threw = false;
    try {
        (void)atperson::load_control_state(path);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok corrupt and invalid files throw\n");
}

void test_save_is_atomic_no_tmp_residue() {
    const auto dir = scratch_dir("atomic");
    const auto path = dir / "control-state.json";

    atperson::ControlState first;
    first.writes_enabled = true;
    atperson::save_control_state(first, path);

    atperson::ControlState second;
    second.paused = true;
    atperson::save_control_state(second, path);

    /* Only the committed file remains; the tmp path never leaks. */
    std::vector<std::filesystem::path> entries;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        entries.push_back(entry.path());
    }
    assert(entries.size() == 1u);
    assert(entries[0] == path);

    /* The committed file is the second state. */
    const auto loaded = atperson::load_control_state(path);
    assert(loaded.paused);
    assert(!loaded.writes_enabled);
    std::printf("ok save is atomic with no tmp residue\n");
}

void test_digest_approval_helpers() {
    auto state = armed_state();
    assert(atperson::is_digest_approved(state, "0123456789abcdef"));
    assert(!atperson::is_digest_approved(state, "ffffffffffffffff"));

    /* Approval list cap keeps the file bounded. */
    state.approved_digests.clear();
    for (std::size_t i = 0u; i < 64u; ++i) {
        char digest[17];
        std::snprintf(digest, sizeof(digest), "%016zx", i);
        state.approved_digests.emplace_back(digest);
    }
    bool threw = false;
    try {
        /* Serialise validates the cap. */
        (void)atperson::serialise_control_state(state);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(!threw);

    state.approved_digests.emplace_back("ffffffffffffffff");
    threw = false;
    try {
        (void)atperson::serialise_control_state(state);
    } catch (const atperson::ControlStateError &) {
        threw = true;
    }
    assert(threw);
    std::printf("ok digest approval helpers\n");
}

} // namespace

int main() {
    test_missing_file_is_fail_closed();
    test_gate_refuses_each_closed_switch();
    test_round_trip_preserves_every_field();
    test_corrupt_and_invalid_files_throw();
    test_save_is_atomic_no_tmp_residue();
    test_digest_approval_helpers();
    return 0;
}
