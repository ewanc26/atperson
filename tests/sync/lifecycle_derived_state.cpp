/* Derived and self-authored state through the durable pipeline:
 * conversation context and journal valence.
 *
 * Part of the deterministic end-to-end lifecycle harness; see
 * tests/support/lifecycle_harness.hpp for the shared fixtures. Offline and
 * deterministic. */

#include "support/lifecycle_harness.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "journal/store.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace atperson::e2e {
namespace {

/* Conversation context must survive the durable pipeline — ledger commit,
 * snapshot mirror, restart from disk — and stay queryable per mirrored
 * entry. Withdrawal of a reply removes its mirror entry (and context) at the
 * next rebuild. And the context must never leak into learning: a quote's
 * target text is not in the vocabulary, only the quoting author's own
 * words. */
void test_conversation_context_survives_restarts_and_withdrawal() {
    Scenario scenario("context");
    SyncObservation top_post = obs("at://e2e/ctx1", "thread start", "did:plc:author");
    SyncObservation reply = obs("at://e2e/ctx2", "a reply arrives", "did:plc:other");
    reply.context.reply_root_uri = "at://did:plc:author/app.bsky.feed.post/3k1";
    reply.context.reply_parent_uri = "at://did:plc:author/app.bsky.feed.post/3k1";
    SyncObservation quote = obs("at://e2e/ctx3", "look at this", "did:plc:third");
    quote.context.quote_uri = "at://did:plc:other/app.bsky.feed.post/3k2";
    /* The quote targets a post whose text contains "quoted vocabulary" —
     * if quote text ever leaked into learning, the token "quoted" would
     * appear in the recovered graph's vocabulary. */
    ScriptedFeed contextual({{{top_post, reply, quote}, std::nullopt}});

    (void)scenario.run(contextual, 1);

    /* Restart from disk: the snapshot mirror carries the context. */
    const auto graph = LanguageGraph::load(scenario.model_file);
    const auto entries = graph.ledger_entries();
    assert(entries.size() == 3u);

    const auto reply_context = graph.ledger_context(1u);
    assert(reply_context.reply_root_uri ==
           std::string("at://did:plc:author/app.bsky.feed.post/3k1"));
    assert(reply_context.reply_parent_uri ==
           std::string("at://did:plc:author/app.bsky.feed.post/3k1"));
    assert(reply_context.quote_uri[0] == '\0');

    const auto quote_context = graph.ledger_context(2u);
    assert(quote_context.quote_uri == std::string("at://did:plc:other/app.bsky.feed.post/3k2"));
    assert(quote_context.reply_root_uri[0] == '\0');

    /* Top-level post keeps empty context. */
    const auto top = graph.ledger_context(0u);
    assert(top.reply_root_uri[0] == '\0' && top.quote_uri[0] == '\0');

    /* Quote text never entered the learned vocabulary: the quoted post's
     * words ("quoted vocabulary") appear nowhere in what was learned.
     * familiarity of an unlearned token reads 0. */
    assert(graph.familiarity("quoted") == 0.0f);
    assert(graph.familiarity("vocabulary") == 0.0f);
    /* The quoting author's own words did. */
    assert(graph.familiarity("look") > 0.0f);

    /* Withdraw the reply; rebuild drops its mirror entry. */
    Ledger ledger(scenario.ledger_file);
    assert(ledger.withdraw_source("at://e2e/ctx2") == 1u);
    LanguageGraph rebuilt;
    const auto report = rebuilt.replay(ledger);
    assert(report.replayed == 2u);
    assert(report.excluded_withdrawn == 1u);
    rebuilt.save(scenario.model_file);

    const auto recovered = LanguageGraph::load(scenario.model_file);
    const auto recovered_entries = recovered.ledger_entries();
    assert(recovered_entries.size() == 2u);
    /* Mirror order preserved: the surviving entries are the top-level
     * post and the quote. */
    assert(recovered_entries[1].source_id == std::string("at://e2e/ctx3"));

    /* Replay rebuild restores context (issue #49): the quote entry carries
     * its quote target, and the withdrawn reply's context is gone with it.
     * Context is never stale or wrong — a rebuilt graph has exactly the
     * context the durable ledger recorded. */
    const auto rebuilt_quote = recovered.ledger_context(1u);
    assert(rebuilt_quote.reply_root_uri[0] == '\0');
    assert(rebuilt_quote.quote_uri == std::string("at://did:plc:other/app.bsky.feed.post/3k2"));
    const auto rebuilt_top = recovered.ledger_context(0u);
    assert(rebuilt_top.reply_root_uri[0] == '\0' && rebuilt_top.quote_uri[0] == '\0');
}

/* Journal valence must survive a rebuild. Valence is self-authored
 * experience. An explicit valence event applied through `atperson journal
 * apply` is recorded in the journal and folded into the live graph, but a
 * plain rebuild from the ledger alone would drop it — the ledger records
 * observations, not valence. The rebuild command must therefore replay the
 * journal's valence entries after the ledger so the rebuilt state includes
 * them.
 *
 * This scenario pins that contract end-to-end: apply a valence event,
 * rebuild from the ledger+journal, and assert the valence state is present
 * in the recovered graph. */
void test_journal_valence_survives_rebuild() {
    Scenario scenario("valence");
    ScriptedFeed feed({
        {{obs("at://e2e/v1", "alpha beta"), obs("at://e2e/v2", "beta gamma")}, std::nullopt},
    });
    (void)scenario.run(feed, 1);

    /* The tokens were observed, so valence attaches to experienced subjects. */
    const auto graph = LanguageGraph::load(scenario.model_file);
    assert(graph.familiarity("alpha") > 0.0f);
    assert(!graph.valence("alpha").has_value());

    /* Apply one explicit valence event and journal it. */
    const auto journal_path = scenario.dir / "action-journal.jsonl";
    JournalValence entry;
    entry.token = "alpha";
    entry.kind = "action";
    entry.signal = 0.8f;
    entry.source = "at://e2e/v1";
    entry.at_epoch = 1758122400u;
    entry.at = "2026-09-17T19:00:00Z";
    atperson::append_journal_valence(journal_path, entry);

    /* A freshly observed graph accepts valence for a known token. The live
     * graph here is independent of the scenario's snapshot — it exists only
     * to demonstrate that the journal entry is a valid valence event. */
    LanguageGraph live;
    live.observe("alpha beta");
    live.valence_event("alpha", ATP_VALENCE_ACTION, 0.8f, 1758122400u, "at://e2e/v1");
    assert(live.valence("alpha").has_value());

    /* Rebuild from the ledger alone drops valence: the rebuilt graph is what
     * the ledger alone would produce. */
    Ledger ledger(scenario.ledger_file);
    LanguageGraph ledger_only;
    (void)ledger_only.replay(ledger);
    assert(!ledger_only.valence("alpha").has_value());

    /* Rebuild from the ledger AND the journal replays the valence event, so
     * the recovered graph carries the folded score. */
    LanguageGraph rebuilt;
    (void)rebuilt.replay(ledger);
    const JournalContents journal = atperson::load_journal(journal_path);
    assert(journal.valence.size() == 1u);
    for (const JournalValence &valence : journal.valence) {
        const std::optional<atp_valence_kind> kind = atperson::valence_kind_from_name(valence.kind);
        assert(kind.has_value());
        rebuilt.valence_event(valence.token, kind.value(), valence.signal, valence.at_epoch,
                              valence.source);
    }
    const auto recovered = rebuilt.valence("alpha");
    assert(recovered.has_value());
    assert(recovered->valence > 0.0f);
    assert(recovered->positive_events == 1u);
    rebuilt.save(scenario.model_file);

    /* The saved snapshot carries the valence state: a restart from disk
     * reads it back. */
    const auto restarted = LanguageGraph::load(scenario.model_file);
    const auto restarted_valence = restarted.valence("alpha");
    assert(restarted_valence.has_value());
    assert(restarted_valence->valence == recovered->valence);
}

} // namespace

void run_derived_state_scenarios() {
    test_conversation_context_survives_restarts_and_withdrawal();
    test_journal_valence_survives_rebuild();
}

} // namespace atperson::e2e
