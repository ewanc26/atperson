/* Property-based persistence round-trip invariants (issue #11).
 *
 * Deterministic property tests over generated inputs — the non-fuzz half
 * of the hardening work. A tiny xorshift generator drives pseudo-random
 * but reproducible observations; the invariants must hold for every input:
 *
 * - snapshot round trip: save -> load yields equivalent learned state
 *   (stats, associations, episodes, mirrored ledger entries);
 * - ledger round trip: append -> reopen yields identical entries and
 *   byte-identical retained payloads;
 * - replay equivalence: a fresh graph replaying the ledger converges to
 *   the same learned state as the graph that observed the text directly.
 *
 * Any violation is a finding; regression inputs become normal tests. */

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/* ---------------------------------------------------------------- */
/* Reproducible pseudo-random generation                             */
/* ---------------------------------------------------------------- */

struct Xorshift {
    std::uint64_t state;
    explicit Xorshift(std::uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::uint32_t below(std::uint32_t bound) { return static_cast<std::uint32_t>(next() % bound); }
};

/* A small vocabulary keeps graphs dense while the structure varies. */
const char *const words[] = {
    "alpha", "beta",  "gamma", "delta", "epsilon", "zeta",  "eta",
    "theta", "iota",  "kappa", "luna",  "sol",     "nox",   "umbra",
    "wolf",  "moon",  "stone", "river", "mist",   "hearth"};
constexpr std::size_t word_count = sizeof(words) / sizeof(words[0]);

std::string random_text(Xorshift &rng, std::size_t max_words) {
    std::string text;
    const std::size_t count = 1u + rng.below(static_cast<std::uint32_t>(max_words));
    for (std::size_t i = 0u; i < count; ++i) {
        if (i > 0u) {
            text += rng.below(4u) == 0u ? ". " : " ";
        }
        text += words[rng.below(word_count)];
    }
    return text;
}

std::string random_uri(Xorshift &rng, std::uint32_t sequence) {
    return "at://property/obs-" + std::to_string(sequence);
}

std::string random_did(Xorshift &rng) {
    return "did:plc:" + std::to_string(rng.below(1000u));
}

std::filesystem::path scratch_dir(const char *tag) {
    const auto root = std::filesystem::temp_directory_path() /
                      std::filesystem::path("atperson-property-" + std::string(tag));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

/* ---------------------------------------------------------------- */
/* Snapshot round-trip property                                      */
/* ---------------------------------------------------------------- */

void test_snapshot_round_trip_equivalence() {
    for (std::uint64_t seed = 1u; seed <= 8u; ++seed) {
        Xorshift rng(seed * 0x2545F4914F6CDD1Dull);
        const auto dir = scratch_dir("snapshot");
        const auto path = dir / "graph.snap";

        atperson::LanguageGraph original;
        for (std::uint32_t i = 0u; i < 40u; ++i) {
            original.observe(random_text(rng, 8u), random_uri(rng, i));
        }
        original.save(path);

        const auto loaded = atperson::LanguageGraph::load(path);
        const auto a = original.stats();
        const auto b = loaded.stats();
        assert(a.node_count == b.node_count);
        assert(a.edge_count == b.edge_count);
        assert(a.training_steps == b.training_steps);
        assert(a.observations == b.observations);

        /* Associations agree for a known token, strongest-first. */
        const auto left = original.associations("alpha", 5u);
        const auto right = loaded.associations("alpha", 5u);
        assert(left.size() == right.size());
        for (std::size_t i = 0u; i < left.size(); ++i) {
            assert(left[i].token == right[i].token);
            assert(left[i].score == right[i].score);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Ledger round-trip property                                        */
/* ---------------------------------------------------------------- */

void test_ledger_round_trip_equivalence() {
    for (std::uint64_t seed = 1u; seed <= 8u; ++seed) {
        Xorshift rng(seed * 0x853C49E6748FEA9Bull);
        const auto dir = scratch_dir("ledger");
        const auto path = dir / "ledger.bin";

        struct Appended {
            std::uint64_t id;
            std::string payload;
        };
        std::vector<Appended> appended;

        {
            atperson::Ledger ledger(path);
            for (std::uint32_t i = 0u; i < 30u; ++i) {
                const std::string payload = random_text(rng, 6u);
                const auto digest = atperson::Ledger::digest(payload);
                std::uint64_t id = 0u;
                const auto result = ledger.append(random_uri(rng, i), random_did(rng),
                                                  rng.next(), digest, ATPERSON_SCHEMA_VERSION,
                                                  ATP_LEDGER_OUTCOME_LEARNED, payload, &id);
                assert(result == atperson::LedgerResult::New);
                appended.push_back({id, payload});
            }
        }

        /* Reopen: entries and payloads survive byte-identical. */
        atperson::Ledger reopened(path);
        assert(reopened.count() == appended.size());
        const auto entries = reopened.entries();
        for (std::size_t i = 0u; i < appended.size(); ++i) {
            assert(entries[i].id == appended[i].id);
            assert(reopened.payload(appended[i].id) == appended[i].payload);
        }

        /* Dedup keys survive: the same (source, digest) pair is a
         * duplicate after reopening. */
        const auto &first = appended.front();
        std::uint64_t id = 0u;
        const auto result = reopened.append(
            random_uri(rng, 0u), "did:plc:x", 0u,
            atperson::Ledger::digest(first.payload), ATPERSON_SCHEMA_VERSION,
            ATP_LEDGER_OUTCOME_LEARNED, first.payload, &id);
        assert(result == atperson::LedgerResult::ExistsCommitted);
    }
}

/* ---------------------------------------------------------------- */
/* Replay equivalence property                                      */
/* ---------------------------------------------------------------- */

void test_replay_converges_to_observed_state() {
    for (std::uint64_t seed = 1u; seed <= 4u; ++seed) {
        Xorshift rng(seed * 0xC0FFEE1234567890ull);
        const auto dir = scratch_dir("replay");
        const auto ledger_path = dir / "ledger.bin";
        const auto snap_path = dir / "graph.snap";

        atperson::LanguageGraph direct;
        {
            atperson::Ledger ledger(ledger_path);
            for (std::uint32_t i = 0u; i < 25u; ++i) {
                const std::string text = random_text(rng, 6u);
                const std::string uri = random_uri(rng, i);
                const auto digest = atperson::Ledger::digest(text);
                std::uint64_t id = 0u;
                const auto result =
                    ledger.append(uri, "did:plc:author", 0u, digest, ATPERSON_SCHEMA_VERSION,
                                  ATP_LEDGER_OUTCOME_LEARNED, text, &id);
                assert(result == atperson::LedgerResult::New);
                direct.remember(text, uri, "did:plc:author", 0u, digest,
                               ATPERSON_SCHEMA_VERSION, id);
            }
            direct.save(snap_path);
        }

        /* A fresh graph replaying the same ledger converges to the same
         * learned state as the graph that observed directly. */
        atperson::Ledger ledger(ledger_path);
        atperson::LanguageGraph rebuilt;
        const auto report = rebuilt.replay(ledger);
        assert(report.replayed == 25u);

        const auto a = direct.stats();
        const auto b = rebuilt.stats();
        assert(a.node_count == b.node_count);
        assert(a.edge_count == b.edge_count);
        assert(a.training_steps == b.training_steps);
        assert(a.observations == b.observations);

        const auto left = direct.associations("alpha", 5u);
        const auto right = rebuilt.associations("alpha", 5u);
        assert(left.size() == right.size());
        for (std::size_t i = 0u; i < left.size(); ++i) {
            assert(left[i].token == right[i].token);
            assert(left[i].score == right[i].score);
        }
    }
}

} // namespace

int main() {
    test_snapshot_round_trip_equivalence();
    test_ledger_round_trip_equivalence();
    test_replay_converges_to_observed_state();

    std::printf("property tests passed\n");
    return 0;
}
