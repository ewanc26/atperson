#ifndef ATPERSON_INGESTION_STATE_HPP
#define ATPERSON_INGESTION_STATE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace atperson {

/* Runtime ingestion source identity. A persisted cursor is only reusable
 * when every field here matches the current session. */
struct IngestionSource {
    std::string kind;
    std::string service;
    std::string account_did;
    std::string endpoint;
    std::optional<std::string> algorithm;
};

/* An interrupted catch-up checkpoint: an opaque AT Protocol feed cursor that
 * a previous traversal stopped at. Never parsed, compared, or treated as
 * evidence that anything was learned — only passed back to Wolfram. */
struct CatchupCursor {
    bool active{};
    std::optional<std::string> cursor;
};

/* Operational telemetry only. Never influences learned state. */
struct IngestionCheckpoint {
    std::uint64_t generation{};
    std::optional<std::string> saved_at;
    std::uint64_t pages_completed{};
    std::uint64_t observations_seen{};
};

struct IngestionState {
    std::uint32_t version{1};
    IngestionSource source;
    CatchupCursor catchup;
    IngestionCheckpoint checkpoint;
};

/* Thrown for malformed state files, unsupported versions, and impossible
 * field combinations. Corruption is reported, never silently reinterpreted. */
class IngestionStateError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/* Canonical service identity: strips a trailing '/' so cosmetic URL
 * differences do not invalidate a stored cursor. */
[[nodiscard]] std::string normalise_service(std::string_view service);

/* The state a fresh runtime starts from: no interrupted traversal, zero
 * counters, source bound to the current session. */
[[nodiscard]] IngestionState initial_ingestion_state(std::string_view service,
                                                     std::string_view account_did);

/* True when every source-identity field matches the current session. A
 * mismatch means the cursor belongs to a different feed and must not be
 * reused. */
[[nodiscard]] bool source_matches(const IngestionState &state,
                                  std::string_view service,
                                  std::string_view account_did);

/* Load and validate the version-1 state file. A missing file yields the
 * clean initial state. Anything else validates fully or throws
 * IngestionStateError. */
[[nodiscard]] IngestionState load_ingestion_state(const std::filesystem::path &path,
                                                  std::string_view service,
                                                  std::string_view account_did);

/* Serialise to a version-1 JSON document (deterministic field order). */
[[nodiscard]] std::string serialise_ingestion_state(const IngestionState &state);

/* Atomically persist a checkpoint: write <path>.tmp, flush, fsync, rename
 * over the committed file, then sync the containing directory. A crash leaves
 * either the previous or the new complete checkpoint, never half-written
 * JSON. Throws std::runtime_error on I/O failure. */
void save_ingestion_state(const IngestionState &state,
                          const std::filesystem::path &path);

/* Explicit operator reset: clears the catch-up cursor and resets traversal
 * counters. Does not touch learned state. */
void reset_ingestion_state(IngestionState &state);

} // namespace atperson

#endif
