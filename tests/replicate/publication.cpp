/* The state-publication pass: the gates, the lock and the bounded drain
 * that both `atperson statepub drain` and the daemon's automatic per-cycle
 * pass run. Offline: the network is a fake OutboundWriter, so the pass is
 * exercised end to end against real files. */

#include "replicate/publication.hpp"

#include "atperson/core.h"
#include "atperson/ledger.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"
#include "replicate/reconstruct.hpp"
#include "resource/runtime.hpp"
#include "state/lock.hpp"
#include "support/fake_pds.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using atperson::ControlState;
using atperson::e2e::FakePds;
using atperson::Ledger;
using atperson::OutboundWriter;
using atperson::PublicationConfig;
using atperson::PublicationPaths;
using atperson::PublicationReport;
using atperson::RuntimeResourceStatus;

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-publication-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/* A host: a data directory with the paths the pass needs. The control file
 * is written explicitly by each scenario, so a test that expects a refusal
 * is never accidentally asserting on a default. */
struct Host {
    std::filesystem::path dir;
    std::filesystem::path ledger_file;
    std::filesystem::path journal_file;
    std::filesystem::path thoughts_dir;
    std::filesystem::path control_file;
    PublicationPaths paths;

    explicit Host(const char *tag)
        : dir(scratch_dir(tag)), ledger_file(dir / "ledger.bin"),
          journal_file(dir / "action-journal.jsonl"), thoughts_dir(dir / "thoughts"),
          control_file(dir / "control-state.json") {
        std::filesystem::create_directories(thoughts_dir);
        paths = PublicationPaths{
            dir,
            atperson::replicate_cursor_path(dir),
            atperson::replicate_records_dir(dir),
            journal_file,
            thoughts_dir,
            control_file,
        };
    }

    void write_control(bool writes_enabled) const {
        ControlState state;
        state.writes_enabled = writes_enabled;
        state.dry_run = false;
        atperson::save_control_state(state, control_file);
    }

    /* One committed observation, so a pass has something to publish. */
    void commit_observation(const std::string &source, const std::string &text) const {
        Ledger ledger(ledger_file);
        std::uint64_t id = 0u;
        (void)ledger.append(source, "did:plc:author", 1758122400u, Ledger::digest(text), 2u,
                            atp_ledger_outcome::ATP_LEDGER_OUTCOME_LEARNED, text, {}, &id);
    }

    [[nodiscard]] std::uint64_t published_count() const {
        return atperson::load_replicate_cursor(paths.cursor_file).next_observation_id - 1u;
    }
};

RuntimeResourceStatus roomy() {
    RuntimeResourceStatus status;
    status.budget.disk_pressure = false;
    return status;
}

PublicationConfig enabled_config() {
    PublicationConfig config;
    config.enabled = true;
    return config;
}

/* The online writer as the daemon and the CLI supply it: a factory, so a
 * pass with no backlog never asks for credentials. */
template <typename Writer> atperson::OnlineWriterFactory factory(Writer &writer) {
    return [&writer]() -> OutboundWriter & { return writer; };
}

void test_writes_gate_refuses_and_retains_the_backlog() {
    Host host("gate");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.write_control(/*writes_enabled=*/false);

    FakePds pds;
    Ledger ledger(host.ledger_file);
    const PublicationReport report = atperson::run_publication_pass(
        host.paths, enabled_config(), roomy(), ledger, false, factory(pds));

    /* Fail-closed: no control file means no gate, and a disabled gate means
     * nothing is published — not "publish anyway, the operator can stop it
     * later". */
    assert(report.refused);
    assert(!report.ran);
    assert(report.refusal.find("writes are disabled") != std::string::npos);
    assert(pds.size() == 0u);
    assert(host.published_count() == 0u);
    std::puts("publication is refused while the write gate is off: ok");
}

void test_a_missing_control_file_also_refuses() {
    /* A fresh host has no control file. It must read as "closed", not as
     * "no gate configured, therefore allowed" — the same fail-closed
     * default every other outbound path uses. */
    Host host("missing-control");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    assert(!std::filesystem::exists(host.control_file));

    FakePds pds;
    Ledger ledger(host.ledger_file);
    const PublicationReport report = atperson::run_publication_pass(
        host.paths, enabled_config(), roomy(), ledger, false, factory(pds));

    assert(report.refused);
    assert(pds.size() == 0u);
    std::puts("a missing control file refuses publication: ok");
}

void test_disk_pressure_refuses_before_touching_the_network() {
    Host host("pressure");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.write_control(true);

    RuntimeResourceStatus tight = roomy();
    tight.budget.disk_pressure = true;
    tight.budget.limiting_disk_path = host.dir;

    FakePds pds;
    Ledger ledger(host.ledger_file);
    const PublicationReport report =
        atperson::run_publication_pass(host.paths, enabled_config(), tight, ledger, false,
                                       factory(pds));

    assert(report.refused);
    assert(pds.size() == 0u);
    assert(host.published_count() == 0u);
    std::puts("disk pressure refuses the pass before any write: ok");
}

void test_a_pass_publishes_the_backlog_and_then_stops() {
    Host host("drain");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.commit_observation("at://did:plc:b/app.bsky.feed.post/2", "second observation text");
    host.write_control(true);

    FakePds pds;
    pds.serve_content("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    pds.serve_content("at://did:plc:b/app.bsky.feed.post/2", "second observation text");

    PublicationConfig config = enabled_config();
    {
        Ledger ledger(host.ledger_file);
        const PublicationReport first = atperson::run_publication_pass(
            host.paths, config, roomy(), ledger, false, factory(pds));
        assert(first.ran);
        assert(!first.refused);
        assert(!first.drain.network_failed);
        assert(first.drain.observations_published == 2u);
    }
    assert(pds.size() == 2u);
    assert(host.published_count() == 2u);

    /* Idempotent: a second pass with nothing new writes nothing. putRecord
     * is keyed on (collection, rkey), so even a republished record would be
     * harmless — but a pass that republishes the whole backlog every cycle
     * would be a rate problem, not a correctness one. */
    {
        Ledger ledger(host.ledger_file);
        const PublicationReport second = atperson::run_publication_pass(
            host.paths, config, roomy(), ledger, false, factory(pds));
        assert(second.ran);
        assert(second.drain.observations_published == 0u);
    }
    assert(pds.size() == 2u);

    /* What was published reconstructs into an equal ledger. */
    const auto fresh = host.dir / "fresh";
    std::filesystem::create_directories(fresh);
    const atperson::ReconstructReport rebuilt = atperson::reconstruct_state(
        pds, fresh / "ledger.bin", fresh / "journal.jsonl", fresh / "thoughts");
    assert(rebuilt.failures.empty());
    assert(rebuilt.observations_replayed == 2u);
    std::puts("a pass drains the backlog once and the records rebuild: ok");
}

void test_offline_stages_the_same_bytes_and_shares_the_cursor() {
    Host host("offline");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.write_control(true);

    FakePds pds;
    {
        Ledger ledger(host.ledger_file);
        const PublicationReport report = atperson::run_publication_pass(
            host.paths, enabled_config(), roomy(), ledger, /*offline=*/true, factory(pds));
        assert(report.ran);
        assert(report.staged_offline);
        assert(!pds.size());
        assert(report.drain.observations_published == 1u);
    }

    /* Staged under the documented layout, one directory per collection. */
    const auto staged = atperson::replicate_records_dir(host.dir);
    assert(std::filesystem::is_directory(staged));
    std::size_t files = 0u;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(staged)) {
        if (entry.is_regular_file()) {
            ++files;
        }
    }
    assert(files == 1u);
    assert(host.published_count() == 1u);

    /* The same cursor: an online pass afterwards resumes from the staged
     * position and has nothing left to publish. */
    pds.serve_content("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    {
        Ledger ledger(host.ledger_file);
        const PublicationReport report = atperson::run_publication_pass(
            host.paths, enabled_config(), roomy(), ledger, /*offline=*/false, factory(pds));
        assert(report.ran);
        assert(!report.staged_offline);
        assert(report.drain.observations_published == 0u);
    }
    std::puts("offline stages the same bytes through the same cursor: ok");
}

void test_a_transport_failure_retains_the_backlog() {
    Host host("transport");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.write_control(true);

    FakePds pds;
    pds.fail_next_put();
    {
        Ledger ledger(host.ledger_file);
        const PublicationReport report = atperson::run_publication_pass(
            host.paths, enabled_config(), roomy(), ledger, false, factory(pds));
        assert(report.ran);
        assert(report.drain.network_failed);
        /* The cursor stays at the last confirmed write: the record is
         * still local, and the next pass republishes it. */
        assert(host.published_count() == 0u);
    }

    {
        Ledger ledger(host.ledger_file);
        const PublicationReport retry = atperson::run_publication_pass(
            host.paths, enabled_config(), roomy(), ledger, false, factory(pds));
        assert(retry.ran);
        assert(!retry.drain.network_failed);
        assert(retry.drain.observations_published == 1u);
    }
    assert(host.published_count() == 1u);
    std::puts("a transport failure retains the backlog for the next pass: ok");
}

void test_a_busy_lock_skips_the_pass() {
    Host host("lock");
    host.commit_observation("at://did:plc:a/app.bsky.feed.post/1", "first observation text");
    host.write_control(true);

    /* Another pass holds the publication lock: this one must skip rather
     * than interleave on the same cursor. */
    const atperson::StateLock held(host.dir, atperson::kStatepubLockName);

    FakePds pds;
    Ledger ledger(host.ledger_file);
    const PublicationReport report = atperson::run_publication_pass(
        host.paths, enabled_config(), roomy(), ledger, false, factory(pds));
    assert(report.refused);
    assert(pds.size() == 0u);
    assert(host.published_count() == 0u);
    std::puts("a busy publication lock skips the pass: ok");
}

void test_cadence_is_explicit_and_off_by_default() {
    PublicationConfig off;
    assert(!off.enabled);
    assert(!atperson::publication_due(off, 1u));
    assert(!atperson::publication_due(off, 100u));

    PublicationConfig every = enabled_config();
    assert(atperson::publication_due(every, 1u));
    assert(atperson::publication_due(every, 2u));

    PublicationConfig every_third = every;
    every_third.every_cycles = 3u;
    assert(!atperson::publication_due(every_third, 1u));
    assert(!atperson::publication_due(every_third, 2u));
    assert(atperson::publication_due(every_third, 3u));
    assert(atperson::publication_due(every_third, 6u));
    /* Cycle 0 is the pre-loop state, not a cycle. */
    assert(!atperson::publication_due(every, 0u));

    /* A hostile variable cannot produce a zero cadence (which would divide
     * by zero) or an unbounded one. */
    setenv("ATPERSON_STATEPUB", "1", 1);
    setenv("ATPERSON_STATEPUB_CYCLES", "0", 1);
    assert(atperson::publication_config_from_environment().every_cycles == 1u);
    setenv("ATPERSON_STATEPUB_CYCLES", "not-a-number", 1);
    assert(atperson::publication_config_from_environment().every_cycles == 1u);
    setenv("ATPERSON_STATEPUB_CYCLES", "999999999", 1);
    assert(atperson::publication_config_from_environment().every_cycles == 100000u);
    unsetenv("ATPERSON_STATEPUB");
    unsetenv("ATPERSON_STATEPUB_CYCLES");
    assert(!atperson::publication_config_from_environment().enabled);

    std::puts("publication cadence is explicit and clamped: ok");
}

} // namespace

void run_publication_scenarios() {
    test_writes_gate_refuses_and_retains_the_backlog();
    test_a_missing_control_file_also_refuses();
    test_disk_pressure_refuses_before_touching_the_network();
    test_a_pass_publishes_the_backlog_and_then_stops();
    test_offline_stages_the_same_bytes_and_shares_the_cursor();
    test_a_transport_failure_retains_the_backlog();
    test_a_busy_lock_skips_the_pass();
    test_cadence_is_explicit_and_off_by_default();
}
