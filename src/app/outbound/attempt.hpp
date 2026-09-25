#ifndef ATPERSON_OUTBOUND_ATTEMPT_HPP
#define ATPERSON_OUTBOUND_ATTEMPT_HPP

// Outbound attempt (#140): the single composition of one execution attempt
// with its durable bookkeeping, shared by the `atperson publish` CLI and the
// autonomous scheduler so both walk exactly the same path.
//
// One attempt is: load gates fresh from disk (control, policy, budget) ->
// execute_outbound_action (pause -> policy -> dry-run -> control -> write) ->
// persist the budget on confirmed success -> append the outbound audit entry
// -> append the journal action (#27 experience provenance).
//
// This atom composes; it never adds a write path. The caller owns locking
// (the outbound lock) and the writer factory. Credentials are consumed only
// by the injected factories, and only when a write is actually reached.
//
// Failure modes: gate/parse errors throw (the caller reports them); a refused
// or failed execution is a normal result, recorded in the audit log and
// journal, never an exception.

#include "action.hpp"
#include "execute.hpp"
#include "journal/mac.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace atperson {

/* Optional per-attempt extensions, all lazy and offline-safe:
 *   `attest`   — badge.blue record attestation factory (publish passes the
 *                environment-backed signer; tests and the scheduler may
 *                leave it unset);
 *   `mac`      — journal-integrity MAC (#57) over the executed record;
 *   `external` — the ATPERSON_ALLOW_EXTERNAL_PUBLISHING gate (default on,
 *                matching the CLI).
 * Both factories must only read credentials when invoked. */
struct OutboundAttemptOptions {
    OutboundAttestationFactory attest{};
    std::function<std::optional<JournalMac>(const OutboundAction &)> mac{};
    bool external_publishing_allowed{true};
};

/* Durable bookkeeping paths for one attempt. */
struct OutboundAttemptPaths {
    std::filesystem::path policy_file;
    std::filesystem::path budget_file;
    std::filesystem::path control_file;
    std::filesystem::path audit_file;
    std::filesystem::path journal_file;
    /* Standing authorization envelopes (#141). Optional: when empty, the
     * attempt is per-digest approval exactly as before. */
    std::filesystem::path envelopes_dir;
    /* Offline spool root (#154). Optional: when empty, no spool-first
     * mirroring happens (tests that do not exercise the spool). When set,
     * every reached network write is spooled before the transport call
     * and removed after confirmed success. */
    std::filesystem::path spool_root;
};

/* Run one attempt end to end. `now` is injected for determinism. The gates
 * are reloaded from disk on every call, so an operator pause, revocation or
 * policy change between attempts always wins. */
[[nodiscard]] OutboundExecutionResult
attempt_outbound_action(const OutboundAction &action, const OutboundAttemptPaths &paths,
                        const OutboundWriterFactory &writer_for, std::int64_t now,
                        const OutboundAttemptOptions &options = {});

} // namespace atperson

#endif
