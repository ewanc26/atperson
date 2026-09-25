/* Offline-safe network writes (#154): a durable local mirror of records the
 * entity created but the network has not confirmed. The spool is the
 * transport-independent half of every outbound write.
 *
 * Design: the spool intercepts at the attempt level, not the writer level.
 * Entries store the canonical serialised `atperson-outbound-action`
 * document — the exact bytes the gates evaluated — so a drain replays
 * through `attempt_outbound_action` and every gate (policy → budget →
 * dry-run → approval → audit) re-runs against the current control state.
 * Reply CIDs resolve online at drain time, not spool time.
 *
 * Durability: entries are written with `write_record` (fsync + rename),
 * zero-padded sequence numbers, creation-order drain. */
#pragma once

#include "attempt.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atperson {

/* A spooled outbound action: the sequence preserves creation order for
 * the drain, and the JSON is the frozen action document. */
struct SpoolEntry {
    std::int64_t seq{};
    std::string action_json;
    std::string spooled_at;
};

/* Parse/serialise a spool entry from/to its on-disk JSON form. */
[[nodiscard]] SpoolEntry parse_spool_entry(const std::string &json);
[[nodiscard]] std::string serialise_spool_entry(const SpoolEntry &entry);

/* Directory layout under the spool root: pending/ and denied/. */
struct SpoolPaths {
    std::filesystem::path root;
    [[nodiscard]] std::filesystem::path pending() const;
    [[nodiscard]] std::filesystem::path denied() const;
};

/* Aggregate counts for inspection. */
struct SpoolStatus {
    std::size_t pending_count{};
    std::size_t denied_count{};
    std::int64_t last_seq{};
    std::optional<std::int64_t> earliest_pending_at;
};

/* Spooling and inspection. */
[[nodiscard]] SpoolStatus spool_status(const SpoolPaths &paths);
[[nodiscard]] std::vector<SpoolEntry> spool_pending(const SpoolPaths &paths);
[[nodiscard]] SpoolEntry spool_append(const SpoolPaths &paths, const std::string &action_json,
                                      const std::string &now_rfc3339);
void spool_remove(const SpoolPaths &paths, const SpoolEntry &entry);
void spool_deny(const SpoolPaths &paths, const SpoolEntry &entry);

/* An OutboundWriter decorator implementing spool-first writes: put_record
 * appends the action-bound record to the spool, delegates to the wrapped
 * writer, and removes the entry on confirmed success. A transport failure
 * leaves the entry spooled — implicit offline, drained later.
 *
 * The entry carries the serialised OutboundAction (not the built record),
 * so a drain re-runs reply CID resolution and every gate exactly as a
 * live attempt would. `action` is the action this write belongs to;
 * `now_rfc3339` stamps the spool time.
 *
 * resolve_record_cid delegates directly: a reply's CID resolution is a
 * read, not a record write, and is not spooled. */
class SpoolFirstWriter final : public OutboundWriter {
  public:
    SpoolFirstWriter(SpoolPaths paths, OutboundWriter &wrapped, OutboundAction action,
                     std::string now_rfc3339);

    std::string resolve_record_cid(const std::string &at_uri) override;
    OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                   const std::string &record_json) override;

  private:
    SpoolPaths paths_;
    OutboundWriter &wrapped_;
    std::string action_json_;
    std::string now_;
};

/* Drain the spool through the exact same composed attempt path as a live
 * write: each pending entry, in creation order, is parsed back into an
 * OutboundAction and replayed via `attempt_outbound_action`. Outcomes:
 *   Executed → remove the entry
 *   Denied   → move to denied/ (permanent, never retried)
 *   DryRun / Deferred → stays pending
 *   Failed   → stop the drain (suffix stays spooled; next drain resumes)
 * The drain stops at the first transport failure so a long outage does
 * not spin. */
struct SpoolDrainReport {
    std::size_t published{};
    std::size_t denied{};
    std::size_t deferred{};
    std::size_t failed{};
    bool transport_failed{false};
    std::string failure_detail;
};

[[nodiscard]] SpoolDrainReport spool_drain(const SpoolPaths &paths,
                                           const OutboundAttemptPaths &attempt_paths,
                                           const OutboundWriterFactory &writer_for,
                                           std::int64_t now);

} // namespace atperson
