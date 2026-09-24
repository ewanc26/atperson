/* Catalog validation: every lexicon under lexicons/ must load into Wolfram's
 * registry and representative records must validate. This protects the
 * click.croft.* records the entity publishes (thoughts, self-eval metrics)
 * from shipping a malformed schema. Network-only because the catalog loader
 * is Wolfram, fetched only in the network build. */

#include <wolfram/validate.h>

#include <cstdio>
#include <string>

namespace {

int report_errors(const wf_validate_result &result) {
    for (const wf_validate_error *error = result.errors; error != nullptr;
         error = error->next) {
        std::fprintf(stderr, "  %s: %s\n", error->path, error->message);
    }
    return result.errors != nullptr ? 1 : 0;
}

bool validate(const wf_lexicon_registry *registry, const char *nsid,
              const std::string &record) {
    const wf_validate_result result =
        wf_validate_record(registry, nsid, record.data(), record.size());
    const int failed = !result.success;
    if (failed) {
        std::fprintf(stderr, "%s: VALIDATION FAILED\n", nsid);
        report_errors(result);
    }
    wf_validate_result_free(const_cast<wf_validate_result *>(&result));
    return !failed;
}

const char *kMetricFull = R"JSON({
  "at": "2026-09-24T19:45:17Z",
  "period_start": "2026-09-17T19:45:17Z",
  "period_end": "2026-09-24T19:45:17Z",
  "cadence_seconds": 604800,
  "actions": {
    "attempts": 12, "executed": 9, "denied": 1, "deferred": 1,
    "failed": 1, "dry_run": 0, "success_rate": 750
  },
  "interaction": {
    "invites": 2, "invites_replied": 1, "invites_expired": 1,
    "invites_pending": 0, "success_rate": 500
  },
  "reply_ratio": { "events": 40, "replies": 23, "ratio": 575 },
  "valence": { "updates": 14, "tokens": 9, "drift": -250 },
  "familiarity": { "authors": 31, "encounters": 212, "new_authors": 6, "accretion": 194 },
  "previous": { "id": "223m8hgzqf3rm", "at": "2026-09-17T19:45:17Z" },
  "trace": {
    "episodes": 84,
    "events": 15,
    "resolutions": 3,
    "valence_updates": 7,
    "new_authors": ["did:plc:aaaa", "did:plc:bbbb"],
    "top_valence": [ { "token": "bpd", "kind": "approach", "sum": 750, "count": 2 } ]
  }
})JSON";

/* The first snapshot omits previous/trace entirely. */
const char *kMetricFirst = R"JSON({
  "at": "2026-09-17T19:45:17Z",
  "period_start": "2026-09-10T19:45:17Z",
  "period_end": "2026-09-17T19:45:17Z",
  "cadence_seconds": 604800,
  "actions": {
    "attempts": 0, "executed": 0, "denied": 0, "deferred": 0,
    "failed": 0, "dry_run": 0, "success_rate": 0
  },
  "interaction": {
    "invites": 0, "invites_replied": 0, "invites_expired": 0,
    "invites_pending": 0, "success_rate": 0
  },
  "reply_ratio": { "events": 0, "replies": 0, "ratio": 0 },
  "valence": { "updates": 0, "tokens": 0, "drift": 0 },
  "familiarity": { "authors": 0, "encounters": 0, "new_authors": 0, "accretion": 0 }
})JSON";

const char *kThought = R"JSON({
  "kind": "consolidation",
  "text": "the wolf howls at a moon that never changes",
  "about": null,
  "at": "2026-09-24T19:45:17Z",
  "span_start": "2026-09-17T19:45:17Z",
  "span_end": "2026-09-24T19:45:17Z",
  "topic": "consolidation"
})JSON";

} // namespace

int main() {
    wf_lexicon_registry *registry = wf_lexicon_registry_new();
    if (registry == nullptr) {
        std::fprintf(stderr, "registry allocation failed\n");
        return 1;
    }
    const wf_status load = wf_lexicon_registry_load_dir(registry, "lexicons");
    if (load != WF_OK) {
        std::fprintf(stderr, "catalog load failed (status %d)\n", static_cast<int>(load));
        wf_lexicon_registry_free(registry);
        return 1;
    }
    if (!wf_lexicon_registry_contains(registry, "click.croft.thought")) {
        std::fprintf(stderr, "catalog missing click.croft.thought\n");
        wf_lexicon_registry_free(registry);
        return 1;
    }
    if (!wf_lexicon_registry_contains(registry, "click.croft.atperson.metric")) {
        std::fprintf(stderr, "catalog missing click.croft.atperson.metric\n");
        wf_lexicon_registry_free(registry);
        return 1;
    }

    bool ok = true;
    ok = validate(registry, "click.croft.atperson.metric", kMetricFull) && ok;
    ok = validate(registry, "click.croft.atperson.metric", kMetricFirst) && ok;
    ok = validate(registry, "click.croft.thought", kThought) && ok;
    wf_lexicon_registry_free(registry);
    return ok ? 0 : 1;
}
