#ifndef ATPERSON_OUTBOUND_AUDIT_HPP
#define ATPERSON_OUTBOUND_AUDIT_HPP

// Outbound execution audit log (#25): an append-only, credential-free record
// of every publish attempt the runtime made.
//
// The durable experience ledger is the authority for *learned* state; this is
// the operator-facing operational record of outbound execution. One compact
// JSON object is appended per attempt, including refusals and dry runs, so the
// sequence of decisions and outcomes is reconstructable after the fact.
//
// The log never contains credentials, tokens or session material. It is not
// part of the model snapshot and not learned C23 state, and #25 does not feed
// it back into learning (that is #27).
//
// Ownership: `OutboundAuditEntry` is payload-by-value. `append_outbound_audit`
// opens, appends one newline-terminated line and flushes before returning.
//
// Failure modes: `std::runtime_error` on I/O failure. A failed append is
// reported loudly; it never rewrites earlier entries.

#include <cstdint>
#include <filesystem>
#include <string>

namespace atperson {

/* What actually happened to one attempted action. */
enum class OutboundAuditOutcome {
    Executed,
    DryRun,
    Denied,
    Deferred,
    Failed,
};

[[nodiscard]] const char *outbound_audit_outcome_name(OutboundAuditOutcome outcome) noexcept;

struct OutboundAuditEntry {
    /* RFC 3339 UTC timestamp of the attempt (injected, not read from a clock
     * inside the execution layer). */
    std::string at;
    std::string kind;
    std::string rkey;
    std::string digest;
    OutboundAuditOutcome outcome{OutboundAuditOutcome::Denied};
    /* Stable machine reason code (policy reason, control gate or "write_failed"). */
    std::string reason;
    /* Human detail; may be empty. */
    std::string detail;
    /* Result URI/CID when the write was executed; empty otherwise. */
    std::string uri;
    std::string cid;
    /* Public proof metadata only; private signing material is never logged. */
    std::string attestation_cid;
    std::string attestation_signature;
    std::string attestation_public_key;
    std::string attestation_key_id;
    std::string attestation_algorithm;
};

/* Deterministic JSON object for one entry (no trailing newline). */
[[nodiscard]] std::string serialise_outbound_audit_entry(const OutboundAuditEntry &entry);

/* Append one newline-terminated entry to `path`, creating the file and its
 * parent directory if needed. Throws std::runtime_error on I/O failure. */
void append_outbound_audit(const std::filesystem::path &path, const OutboundAuditEntry &entry);

} // namespace atperson

#endif
