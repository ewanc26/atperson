#include "scheduler/drives.hpp"

#include "intent/state.hpp"
#include "journal/store.hpp"
#include "state/time.hpp"
#include "atperson/tokenize.h"

#include <algorithm>
#include <string_view>

namespace atperson {
namespace drives {
namespace {

/* Number of remembered episodes authored by `author_did`. Episodes are the
 * authoritative learned record of who the entity has encountered; no second
 * counter store is introduced. */
std::size_t author_encounters(const LanguageGraph &graph, std::string_view author_did) {
    if (author_did.empty()) {
        return 0u;
    }
    std::size_t count = 0u;
    for (const atp_episode &episode : graph.episodes()) {
        if (std::string_view(episode.author_did) == author_did) {
            ++count;
        }
    }
    return count;
}

/* Encounter-based author curiosity: novelty * adjacency, where novelty is
 * 1/(1+encounters) and adjacency is whether the author was ever encountered.
 * An unseen author reads zero; a heavily-encountered one decays toward zero. */
float author_curiosity(const LanguageGraph &graph, std::string_view author_did) {
    const std::size_t encounters = author_encounters(graph, author_did);
    if (encounters == 0u) {
        return 0.0f;
    }
    return 1.0f / static_cast<float>(1u + encounters);
}

/* One pass over the candidate payload: count tokens, and accumulate
 * familiarity over the known ones only. Tokenisation never mutates the
 * graph or the vocabulary. */
struct TopicCollector {
    const LanguageGraph *graph{};
    float familiarity_sum{};
    std::size_t total{};
    std::size_t known{};
};

bool emit_token(void *userdata, const char *token) {
    auto *collector = static_cast<TopicCollector *>(userdata);
    ++collector->total;
    if (collector->graph->has_token(token)) {
        ++collector->known;
        collector->familiarity_sum += collector->graph->familiarity(token);
    }
    return true;
}

/* Token-level curiosity: novelty * adjacency, where novelty is
 * 1/(1 + mean familiarity) over the known tokens — the same bounded [0, 1]
 * form as the author/interaction familiarity f/(f+1). A fresh graph has no
 * known tokens (0); heavily-exposed tokens drive novelty toward zero; a
 * partially-known subject lands strictly between. */
float topic_curiosity(const LanguageGraph &graph, std::string_view text) {
    if (text.empty()) {
        return 0.0f;
    }
    TopicCollector collector{&graph, 0.0f, 0u, 0u};
    const std::string owned(text);
    atp_tokenize(owned.c_str(), ATPERSON_SCHEMA_VERSION, emit_token, &collector);
    if (collector.total == 0u || collector.known == 0u) {
        return 0.0f;
    }
    const float mean_familiarity = collector.familiarity_sum / static_cast<float>(collector.known);
    const float novelty = 1.0f / (1.0f + mean_familiarity);
    const float adjacency = static_cast<float>(collector.known) / static_cast<float>(collector.total);
    return std::clamp(novelty * adjacency, 0.0f, 1.0f);
}

} // namespace

std::vector<ContextCandidate> select_candidates(const Ledger &ledger, std::size_t max_contexts) {
    std::vector<ContextCandidate> candidates;
    candidates.reserve(max_contexts);
    const std::vector<atp_ledger_entry> entries = ledger.entries();
    for (auto it = entries.rbegin(); it != entries.rend() && candidates.size() < max_contexts;
         ++it) {
        /* Only committed outcomes are durable observation authority:
         * PENDING entries are mid-pipeline and WITHDRAWN ones are durably
         * gone (deleted sources are never decided on). */
        if (it->outcome == ATP_LEDGER_OUTCOME_PENDING ||
            it->outcome == ATP_LEDGER_OUTCOME_WITHDRAWN) {
            continue;
        }
        /* Payload-less entries (v1-migrated) carry nothing to decide on. */
        const std::string payload = ledger.payload(it->id);
        if (payload.empty()) {
            continue;
        }
        ContextCandidate candidate;
        candidate.ledger_id = it->id;
        candidate.payload = payload;
        candidate.source_id = it->source_id;
        candidate.author_did = it->author_did;
        candidate.observed_at = it->observed_at;
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::vector<Signals> compute_drive_signals(const LanguageGraph &graph,
                                           const std::vector<ContextCandidate> &candidates,
                                           const JournalContents &journal, std::int64_t now) {
    std::vector<Signals> out;
    out.reserve(candidates.size());
    for (const ContextCandidate &candidate : candidates) {
        Signals signals;
        signals.curiosity =
            std::clamp(std::max(author_curiosity(graph, candidate.author_did),
                                topic_curiosity(graph, candidate.payload)),
                       0.0f, 1.0f);

        /* Reciprocity: the entity acted, and a public record referenced that
         * action. The strongest signal is the candidate being exactly the
         * referencing record; the weaker one is its author having referenced
         * the entity's action inside a bounded window. The event timestamp
         * that fails to parse is outside the window by construction. */
        const std::int64_t window_lower = now - kReciprocityWindowSeconds;
        const std::int64_t window_upper = now + 60;
        for (const JournalEvent &event : journal.events) {
            if (candidate.source_id == event.event_uri) {
                signals.reciprocity = 1.0f;
                signals.reciprocity_source = kReciprocityEvent;
                break;
            }
            if (signals.reciprocity_source == kReciprocityNone && !candidate.author_did.empty() &&
                event.author_did == candidate.author_did) {
                const std::int64_t event_epoch =
                    static_cast<std::int64_t>(parse_rfc3339_epoch(event.at).value_or(0ull));
                if (event_epoch >= window_lower && event_epoch <= window_upper) {
                    signals.reciprocity = 0.5f;
                    signals.reciprocity_source = kReciprocityAuthor;
                }
            }
        }

        /* Intent (#150): an open pending intent is waiting on this exactly
         * record — the reply the entity invited. Full strength when it
         * matches, zero otherwise; the drive never fabricates one. */
        signals.intent =
            continuation_intent(journal, candidate.source_id, candidate.author_did, now) != nullptr
                ? 1.0f
                : 0.0f;

        out.push_back(signals);
    }
    return out;
}

std::vector<std::size_t> order_candidates(const std::vector<Signals> &signals) {
    std::vector<std::size_t> indices(signals.size());
    for (std::size_t i = 0u; i < indices.size(); ++i) {
        indices[i] = i;
    }
    std::stable_sort(indices.begin(), indices.end(), [&signals](std::size_t a, std::size_t b) {
        if (signals[a].intent != signals[b].intent) {
            return signals[a].intent > signals[b].intent;
        }
        if (signals[a].reciprocity != signals[b].reciprocity) {
            return signals[a].reciprocity > signals[b].reciprocity;
        }
        if (signals[a].curiosity != signals[b].curiosity) {
            return signals[a].curiosity > signals[b].curiosity;
        }
        return false;
    });
    return indices;
}

} // namespace drives
} // namespace atperson