#ifndef ATPERSON_REPLICATE_PUBLICATION_HPP
#define ATPERSON_REPLICATE_PUBLICATION_HPP

// One state-publication pass: the gates, the lock and the drain that
// publish committed experience as AT Protocol records (#142), and the
// configuration that decides whether an unattended daemon runs one.
//
// The publication pass is the *only* way durable experience leaves this
// host, so where it lives and who may run it matters:
//
//   - it is an outbound network write procedure, and the master write gate
//     applies exactly as it does to outbound actions. There is no
//     publication path around the control state.
//   - it is bounded per pass. A backlog drains across passes, never in one
//     unbounded write burst.
//   - offline stages the exact record bytes to the data directory through
//     the same cursor and the same gates, so an offline host still
//     accumulates a complete, inspectable record set.
//   - progress is a durable cursor, so an interrupted pass resumes exactly
//     where it stopped and putRecord idempotency makes a retry safe.
//
// Both callers share this one implementation: the operator-driven
// `atperson statepub drain`, and the daemon's automatic per-cycle pass. They
// cannot drift, because there is nothing to drift — a refusal here is the
// same refusal in both.
//
// Network boundary: this module never opens a session. The online writer is
// injected by the caller (Wolfram in the runtime, a fake in tests), and is
// requested lazily, so a pass with no backlog reads no credentials.
// Compiled into both network-off and network-on builds.

#include "atperson/ledger.hpp"
#include "outbound/execute.hpp"
#include "publish.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace atperson {

struct RuntimeResourceStatus;

/* The dedicated publication lockfile. It is separate from the daemon's
 * long-held writer lock so a drain never competes with a running daemon for
 * state-directory ownership. */
inline constexpr const char *kStatepubLockName = ".statepub-lock";

/* Durable publication cursor, shared by online drains, offline staging and
 * the daemon's automatic pass. */
[[nodiscard]] std::filesystem::path replicate_cursor_path(const std::filesystem::path &data_dir);

/* Where offline staging lands: `<data>/replicate/records/<collection>/<rkey>.json`. */
[[nodiscard]] std::filesystem::path replicate_records_dir(const std::filesystem::path &data_dir);

/* The paths one pass needs. The caller owns them; the pass opens no state
 * directory of its own. */
struct PublicationPaths {
    std::filesystem::path data_dir;
    std::filesystem::path cursor_file;
    std::filesystem::path records_dir;
    std::filesystem::path journal_file;
    std::filesystem::path thoughts_dir;
    std::filesystem::path control_file;
};

/* Bounds for the daemon's automatic pass. Same shape as the operator
 * command's bounds, so an unattended pass cannot be more aggressive than a
 * deliberate one. */
struct PublicationConfig {
    /* ATPERSON_STATEPUB=1: the daemon runs a pass itself. Off by default —
     * an unattended host must not start publishing because a variable was
     * set once, and the operator command remains available either way. */
    bool enabled{false};
    /* Records written per pass. */
    std::uint32_t max_records_per_drain{50u};
    /* How far back to recheck published entries for withdrawal changes. */
    std::uint32_t withdrawal_recheck_window{200u};
    /* ATPERSON_STATEPUB_CYCLES: run one pass every N daemon cycles. */
    std::uint32_t every_cycles{1u};
};

/* ATPERSON_STATEPUB=1, ATPERSON_STATEPUB_CYCLES,
 * ATPERSON_STATEPUB_MAX_RECORDS, ATPERSON_STATEPUB_RECHECK_WINDOW.
 * Every value is clamped; a hostile or mistaken variable cannot produce an
 * unbounded or zero pass. */
[[nodiscard]] PublicationConfig publication_config_from_environment();

/* Whether a pass is due on this (1-based) cycle. False when the switch is
 * off. The cadence is explicit rather than "every cycle", so an operator
 * can choose the rate at which durable experience reaches the network. */
[[nodiscard]] bool publication_due(const PublicationConfig &config,
                                   std::uint64_t cycle) noexcept;

struct PublicationReport {
    /* A drain actually ran. */
    bool ran{false};
    /* A gate refused it. Not an error: the backlog stays local and the next
     * pass retries. `refusal` says which gate. */
    bool refused{false};
    std::string refusal;
    /* The pass staged to the data directory instead of the network. */
    bool staged_offline{false};
    ReplicateReport drain;
};

/* The online writer, requested only when the pass is about to write. */
using OnlineWriterFactory = std::function<OutboundWriter &()>;

/* One bounded pass.
 *
 * `offline` stages instead of publishing. The daemon passes the control
 * state's offline flag; the CLI passes its `--offline` flag, which can only
 * ever be more conservative than the flag's absence.
 *
 * `ledger` is the caller's open handle: the daemon's own, or one opened for
 * the command. The pass never opens a second handle to the same ledger.
 *
 * Never throws. A missing write gate, insufficient disk headroom, a busy
 * lock, a corrupt control file and a network failure are all reported in the
 * result, because every caller is a long-running loop that must not die of
 * an ordinary condition. */
[[nodiscard]] PublicationReport run_publication_pass(
    const PublicationPaths &paths, const PublicationConfig &config,
    const RuntimeResourceStatus &resource_status, Ledger &ledger, bool offline,
    const OnlineWriterFactory &online_writer);

} // namespace atperson

#endif // ATPERSON_REPLICATE_PUBLICATION_HPP
