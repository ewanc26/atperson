#ifndef ATPERSON_SELFEVAL_STORE_HPP
#define ATPERSON_SELFEVAL_STORE_HPP

// The schema-versioned longitudinal metric snapshot record (#153), one record
// per file in a flat directory. Record keys are TIDs, so id order is append
// order; `previous` names the immediately preceding snapshot in the series.
// Deltas are computed on the display path between neighbouring snapshots, so a
// stored record is a single self-contained period and the full weekly series
// is reconstructable from the records alone.
//
// Store JSON mirrors the network-native record published as
// click.croft.atperson.metric (same `format`/`version` envelope). Serialised
// with cJSON; malformed records throw MetricError; I/O failure throws
// std::runtime_error. A missing directory yields an empty series.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

inline constexpr std::uint32_t kMetricFormatVersion = 1u;

/* Malformed or unsupported metric snapshot content. Corruption is reported,
 * never silently reinterpreted. */
class MetricError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Action execution: attempts vs. outcomes, cumulative over all time. */
struct MetricActions {
    std::size_t attempts{0u};
    std::size_t executed{0u};
    std::size_t denied{0u};
    std::size_t deferred{0u};
    std::size_t failed{0u};
    std::size_t dry_run{0u};
    double success_rate{0.0}; /* executed / attempts */
};

/* Pending social intents (#150): terminal intent outcomes by state. */
struct MetricInteraction {
    std::size_t invites{0u};
    std::size_t invites_replied{0u};  /* state Closed: budget reached */
    std::size_t invites_expired{0u};  /* state Expired: window closed */
    std::size_t invites_pending{0u};  /* state Open: still being waited on */
    double success_rate{0.0}; /* replied / (replied + expired), terminal only */
};

/* Public reply/quote referencing of the entity's executed actions. */
struct MetricReplyRatio {
    std::size_t events{0u};
    std::size_t replies{0u}; /* via == "reply" */
    double ratio{0.0};       /* replies / events */
};

/* Explicit valence state updates (#13), cumulative. */
struct MetricValence {
    std::size_t updates{0u};
    std::size_t tokens{0u}; /* distinct tokens with any update */
    double drift{0.0};      /* net signal sum across all updates */
};

/* Observed author exposure, cumulative, from the ledger's LEARNED entries. */
struct MetricFamiliarity {
    std::size_t authors{0u};     /* distinct authors seen at least once */
    std::size_t encounters{0u};  /* total author-bearing learned entries */
    std::size_t new_authors{0u}; /* authors first seen in this period */
    double accretion{0.0};       /* new_authors / authors */
};

/* One bounded window-trace group: valence movement per (token, kind). */
struct MetricGroup {
    std::string token;
    std::string kind;
    double signal_sum{0.0};
    std::size_t count{0u};
};

/* Bounded trace of the state each metric summarises: an ordered snapshot of
 * the period's activity so every number carries provenance. */
struct MetricTrace {
    std::size_t episodes{0u};          /* learned episodes in the period */
    std::size_t events{0u};            /* linked public events in the period */
    std::size_t resolutions{0u};       /* expectation resolutions in the period */
    std::size_t valence_updates{0u};   /* valence updates in the period */
    std::vector<std::string> new_authors;         /* bounded, sorted */
    std::vector<MetricGroup> top_valence;         /* bounded, |sum| descending */
};

struct MetricSnapshot {
    std::string id;
    std::string at; /* RFC 3339 when the pass ran */
    std::string period_start;
    std::string period_end;
    std::uint64_t cadence_seconds{0u};

    MetricActions actions;
    MetricInteraction interaction;
    MetricReplyRatio reply_ratio;
    MetricValence valence;
    MetricFamiliarity familiarity;
    MetricTrace trace;

    bool has_previous{false};
    std::string previous_id;
    std::string previous_at;
};

/* Deterministic JSON for the snapshot (no trailing newline). Throws
 * MetricError when the snapshot cannot be represented safely. */
[[nodiscard]] std::string serialise_metric_snapshot(const MetricSnapshot &snapshot);

[[nodiscard]] MetricSnapshot parse_metric_snapshot(std::string_view json);

/* Atomic append; refuses to overwrite an existing record id. */
void write_metric_snapshot(const std::filesystem::path &dir, const MetricSnapshot &snapshot);

/* Every snapshot under `dir`, ascending (append) order. A torn empty file is
 * skipped; a malformed complete file throws MetricError. */
[[nodiscard]] std::vector<MetricSnapshot> load_metric_snapshots(const std::filesystem::path &dir);

} // namespace atperson

#endif