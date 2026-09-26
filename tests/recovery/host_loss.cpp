/* Issue #143 host loss: the kill-the-host drill.
 *
 * Part of the deterministic end-to-end lifecycle harness; see
 * tests/support/lifecycle_harness.hpp for the shared fixtures. Offline
 * and deterministic: the network is a FakePds in memory, so the only thing
 * that survives the wipe is what was published.
 *
 * The drill is the acceptance test for persistent hosting, and it is
 * deliberately brutal. The original host ingests, journals self-authored
 * experience, and publishes. Then the entire data directory is deleted —
 * ledger, snapshot, journal, thoughts, cursors, control state, run state,
 * everything. A second, empty host reconstructs from the network copy
 * alone and must arrive at the same learned state. */

#include "support/fake_pds.hpp"
#include "support/lifecycle_harness.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "control/state.hpp"
#include "journal/store.hpp"
#include "replicate/publish.hpp"
#include "replicate/reconstruct.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace atperson::e2e {
namespace {

/* A host-independent fingerprint of what the entity has learned: durable
 * ledger outcomes, the vocabulary with its familiarity, the associations,
 * and valence.
 *
 * Snapshot bytes are deliberately not part of this. The two hosts are
 * different machines, and a snapshot is a portable persisted artefact whose
 * layout may legitimately differ (capacity, ordering) while the learned
 * state is identical. Asserting on bytes would pin the drill to a
 * serialisation rather than to the thing the runbook promises: the
 * experience survives. */
std::string learned_state_fingerprint(const Scenario &scenario) {
    std::ostringstream out;
    const Ledger ledger(scenario.ledger_file);
    for (const auto &entry : ledger.entries()) {
        out << "entry " << entry.id << '|' << entry.source_id << '|' << entry.author_did << '|'
            << entry.observed_at << '|' << entry.content_digest << '|' << entry.outcome << '\n';
    }

    const auto graph = std::filesystem::exists(scenario.model_file)
                           ? LanguageGraph::load(scenario.model_file)
                           : LanguageGraph();

    const auto stats = graph.stats();
    out << "stats " << stats.observations << '|' << stats.node_count << '|' << stats.edge_count
        << '\n';

    /* familiar_tokens is already strongest-first with a documented
     * tie-break; sort anyway so the fingerprint cannot depend on an
     * ordering guarantee the recovery path does not need. */
    auto tokens = graph.familiar_tokens(4096u);
    std::sort(tokens.begin(), tokens.end());
    for (const auto &[token, familiarity] : tokens) {
        out << "token " << token << '|' << familiarity << '\n';
    }

    /* Associations, so a rebuild that learned the same tokens but a
     * different graph structure does not slip through. */
    for (const auto &[token, familiarity] : tokens) {
        (void)familiarity;
        auto links = graph.associations(token, 64u);
        std::sort(links.begin(), links.end(),
                  [](const auto &left, const auto &right) { return left.token < right.token; });
        for (const auto &link : links) {
            out << "edge " << token << "->" << link.token << '|' << link.score << '|'
                << link.observations << '\n';
        }
    }

    auto valences = graph.valence_records();
    std::sort(valences.begin(), valences.end(),
              [](const auto &left, const auto &right) { return left.token < right.token; });
    for (const auto &valence : valences) {
        out << "valence " << valence.token << '|' << valence.valence << '|'
            << valence.positive_events << '|' << valence.negative_events << '\n';
    }

    const auto entries = graph.ledger_entries();
    for (const auto &entry : entries) {
        out << "mirror " << entry.id << '|' << entry.source_id << '|' << entry.content_digest
            << '\n';
    }
    return out.str();
}

/* The feed text, kept next to the feed itself so the "source" the
 * reconstruct path re-fetches from can be served with exactly the bytes
 * the entity originally learned. A digest that does not match is refused,
 * so this map is load-bearing: without it nothing may be replayed. */
const std::map<std::string, std::string> &source_texts() {
    static const std::map<std::string, std::string> texts{
        {"at://e2e/host1", "alpha beta alpha beta"},
        {"at://e2e/host2", "beta gamma"},
        {"at://e2e/host3", "gamma delta gamma delta"},
    };
    return texts;
}

ScriptedFeed host_feed() {
    return ScriptedFeed({
        {{obs("at://e2e/host1", "alpha beta alpha beta"), obs("at://e2e/host2", "beta gamma"),
          obs("at://e2e/host3", "gamma delta gamma delta")},
         std::nullopt},
    });
}

/* Fold the journal's valence entries into the graph, exactly as
 * `atperson rebuild` does after replaying the ledger. The ledger records
 * observations; valence is self-authored experience that lives only in the
 * journal, so a rebuild that skipped it would quietly lose it. */
void fold_journal_valence(LanguageGraph &graph, const std::filesystem::path &journal_file) {
    const JournalContents journal = load_journal(journal_file);
    for (const JournalValence &valence : journal.valence) {
        const std::optional<atp_valence_kind> kind = valence_kind_from_name(valence.kind);
        assert(kind.has_value());
        graph.valence_event(valence.token, kind.value(), valence.signal, valence.at_epoch,
                            valence.source);
    }
}

void test_host_loss_recovers_learned_state_from_the_network() {
    FakePds pds;

    /* ---- the original host ------------------------------------------ */
    Scenario original("host-loss-original");
    (void)original.run(host_feed(), 1);
    {
        Ledger ledger(original.ledger_file);
        assert(ledger.count() == 3u);
    }

    /* Self-authored experience: a valence event in the journal, which the
     * ledger alone would not carry. */
    JournalValence valence;
    valence.token = "alpha";
    valence.kind = "action";
    valence.signal = 0.75f;
    valence.source = "at://e2e/host1";
    valence.at_epoch = 1758122400u;
    valence.at = "2026-09-17T19:00:00Z";
    append_journal_valence(original.journal_file(), valence);

    /* `atperson journal apply` folds the event into the live graph as well
     * as journalling it, so the original host's own snapshot carries it. A
     * host that journalled without folding would not be a state the
     * recovery path is ever asked to match. */
    {
        auto graph = LanguageGraph::load(original.model_file);
        graph.valence_event("alpha", ATP_VALENCE_ACTION, 0.75f, valence.at_epoch, valence.source);
        graph.save(original.model_file);
    }

    const std::string before = learned_state_fingerprint(original);
    assert(!before.empty());
    const auto before_valence = LanguageGraph::load(original.model_file).valence("alpha");
    assert(before_valence.has_value());
    assert(before_valence->positive_events == 1u);

    /* ---- publish: the only thing that will survive ------------------ */
    const std::filesystem::path replicate_cursor = original.dir / "replicate-cursor.json";
    const std::filesystem::path thoughts_dir = original.dir / "thoughts";
    {
        Ledger ledger(original.ledger_file);
        ReplicateConfig config;
        const ReplicateReport published = replicate_drain(replicate_cursor, original.journal_file(),
                                                          ledger, thoughts_dir, pds, config);
        assert(!published.network_failed);
        assert(published.observations_published == 3u);
        assert(published.valence_published == 1u);
    }
    assert(pds.size() == 4u);
    /* The network holds provenance, never the observed text. */
    pds.serve_all_contents(source_texts());

    /* ---- kill the host ---------------------------------------------- */
    std::filesystem::remove_all(original.dir);
    assert(!std::filesystem::exists(original.dir));

    /* ---- the fresh host --------------------------------------------- */
    Scenario fresh("host-loss-fresh");
    const ReconstructReport rebuilt =
        reconstruct_state(pds, fresh.ledger_file, fresh.journal_file(), fresh.dir / "thoughts");
    assert(rebuilt.failures.empty());
    assert(rebuilt.records_corrupt == 0u);
    assert(rebuilt.observations_failed == 0u);
    assert(rebuilt.observations_replayed == 3u);
    assert(rebuilt.valence_replayed == 1u);

    /* Rebuild the model the way `atperson rebuild` does. */
    {
        Ledger ledger(fresh.ledger_file);
        LanguageGraph graph;
        const auto replay = graph.replay(ledger);
        assert(replay.replayed == 3u);
        assert(replay.excluded_withdrawn == 0u);
        fold_journal_valence(graph, fresh.journal_file());
        graph.save(fresh.model_file);
    }

    /* The acceptance criterion: the learned state survived. */
    const std::string after = learned_state_fingerprint(fresh);
    assert(after == before);

    /* Specifically: the experience is still queryable, and the journal-only
     * valence came back with it. A fingerprint that merely matched because
     * both sides were empty would pass the assert above, so pin the
     * substance. */
    const auto recovered = LanguageGraph::load(fresh.model_file);
    assert(recovered.stats().observations == 3u);
    assert(recovered.familiarity("alpha") > 0.0f);
    assert(recovered.familiarity("delta") > 0.0f);
    const auto recovered_valence = recovered.valence("alpha");
    assert(recovered_valence.has_value());
    /* The event arrived, and it moved the score by exactly what it moved on
     * the host that lost it. (The stored score is the rate-smoothed signal,
     * not the raw one, so the value itself is not 0.75.) */
    assert(recovered_valence->valence == before_valence->valence);
    assert(recovered_valence->positive_events == 1u);
    assert(recovered_valence->negative_events == 0u);

    std::puts("host loss: learned state recovered from the network alone: ok");
}

void test_fresh_host_starts_closed_after_recovery() {
    /* The other half of the runbook's promise: a rebuilt host is not a
     * restored host. Nothing local survived, so there is no control file
     * and the fail-closed defaults must apply — the entity learns and
     * publishes nothing until an operator re-enables writes. */
    const std::filesystem::path dir = scratch_dir("host-loss-closed");
    const auto control_file = dir / "control-state.json";
    assert(!std::filesystem::exists(control_file));

    const ControlState state = load_control_state(control_file);
    assert(!state.writes_enabled);
    assert(state.dry_run);
    assert(state.approval_required);
    assert(!state.paused);
    assert(state.approved_digests.empty());

    std::puts("fresh host after recovery starts closed: ok");
}

/* A host that lost an un-drained backlog loses it permanently, and must
 * know it. The network copy is the recovery boundary; reconstructing from
 * it yields a shorter history than the lost host had, and the drill must
 * report that as a difference rather than quietly matching. */
void test_undrained_backlog_is_a_recovery_boundary() {
    FakePds pds;
    Scenario original("host-loss-backlog");
    (void)original.run(host_feed(), 1);

    /* Drain only part of what was observed: the rest is still local. */
    {
        Ledger ledger(original.ledger_file);
        ReplicateConfig config;
        config.max_records_per_drain = 2u;
        const ReplicateReport first =
            replicate_drain(original.dir / "replicate-cursor.json", original.journal_file(), ledger,
                            original.dir / "thoughts", pds, config);
        assert(!first.network_failed);
        assert(first.observations_published == 2u);
    }
    pds.serve_all_contents(source_texts());

    const std::string before = learned_state_fingerprint(original);
    std::filesystem::remove_all(original.dir);

    Scenario fresh("host-loss-backlog-fresh");
    const ReconstructReport rebuilt =
        reconstruct_state(pds, fresh.ledger_file, fresh.journal_file(), fresh.dir / "thoughts");
    assert(rebuilt.failures.empty());
    assert(rebuilt.observations_replayed == 2u);
    {
        Ledger ledger(fresh.ledger_file);
        LanguageGraph graph;
        (void)graph.replay(ledger);
        graph.save(fresh.model_file);
    }

    /* Two observations came back, not three. This is the documented
     * boundary, and it is why the runbook says reconstruction is only as
     * current as the last drain. */
    const std::string after = learned_state_fingerprint(fresh);
    assert(after != before);
    const auto recovered = LanguageGraph::load(fresh.model_file);
    assert(recovered.stats().observations == 2u);

    std::puts("undrained backlog is a recovery boundary, not silent loss: ok");
}

} // namespace

void run_host_loss_scenarios() {
    test_host_loss_recovers_learned_state_from_the_network();
    test_fresh_host_starts_closed_after_recovery();
    test_undrained_backlog_is_a_recovery_boundary();
}

} // namespace atperson::e2e
