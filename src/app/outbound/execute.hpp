#ifndef ATPERSON_OUTBOUND_EXECUTE_HPP
#define ATPERSON_OUTBOUND_EXECUTE_HPP

// Outbound execution (#25): the single, strictly ordered path from an approved
// action document to one network write.
//
// Gate order, all fail-closed:
//   1. operator pause        (refuse before any evaluation)
//   2. #23 outbound policy    (allow / deny / defer, with rate budgets)
//   3. dry-run                (evaluate fully, then stop)
//   4. #22 control gate       (writes_enabled + exact-digest approval)
//   5. network write          (via the injected OutboundWriter)
//
// A write is attempted only after every gate passes. The budget is recorded
// only on a *confirmed* success: an ambiguous failure deliberately leaves the
// budget untouched so the operator may retry the same frozen rkey, which is
// idempotent, rather than silently losing the action.
//
// The network itself is behind the `OutboundWriter` interface. This translation
// unit has no Wolfram dependency, so the ordering, fail-closed behaviour and
// budget bookkeeping are covered by offline tests; the Wolfram-backed adapter
// lives in the network-scoped runtime.
//
// Ownership: `execute_outbound_action` borrows the policy, mutates the passed
// budget state on success, and borrows the writer. `OutboundWriterFactory` is
// invoked at most once, and only if a write is actually reached, so a dry run
// or refusal never logs in.

#include "action.hpp"
#include "budget.hpp"
#include "config.hpp"
#include "control/state.hpp"
#include "control/envelope.hpp"
#include "evaluate.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace atperson {

/* Result of one write: the created/replaced record's at-URI and CID. */
struct OutboundWriteResult {
    std::string uri;
    std::string cid;
};

/* The network boundary. Implementations translate the already-built record
 * body into an AT Protocol write; they never decide policy, never mutate
 * budgets and never build the record text. */
class OutboundWriter {
  public:
    virtual ~OutboundWriter() = default;

    /* Resolve the content CID of an existing record, for a reply strongRef. */
    virtual std::string resolve_record_cid(const std::string &at_uri) = 0;

    /* putRecord the exact body under the frozen rkey (idempotent on retry). */
    virtual OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                           const std::string &record_json) = 0;
};

/* Lazily supplies the writer, so no session is established unless a write is
 * actually reached. Must return a reference valid for the call. */
using OutboundWriterFactory = std::function<OutboundWriter &()>;

struct OutboundAttestation {
    std::string payload_cid;
    std::string signature_hex;
    std::string public_key_did;
    std::string key_id;
    std::string algorithm;
};

using OutboundAttestationFactory =
    std::function<std::optional<OutboundAttestation>(const std::string &record_json,
                                                      const OutboundAction &action)>;

enum class OutboundExecutionOutcome { Executed, DryRun, Denied, Deferred, Failed };

[[nodiscard]] const char *
outbound_execution_outcome_name(OutboundExecutionOutcome outcome) noexcept;

/* How the #22 control gate was satisfied for one execution. Either the
 * operator approved the exact digest, or a standing authorization envelope
 * (#141) covered the action at execution time. `envelope_id` is set only
 * for envelope authorisation, so the audit log can reconstruct which
 * standing rule applied. */
struct OutboundAuthorization {
    bool digest_approved{false};
    std::string envelope_id;
};

struct OutboundExecutionResult {
    OutboundExecutionOutcome outcome{OutboundExecutionOutcome::Denied};
    /* Stable machine reason: a #23 reason code, a control code ("paused",
     * "writes_disabled", "approval_required", "dry_run") or "write_failed". */
    std::string reason_code;
    std::string detail;
    /* Populated only when outcome == Executed. */
    OutboundWriteResult written{};
    std::optional<OutboundAttestation> attestation;
    /* True only when the write was confirmed and the budget consumed. */
    bool budget_recorded{false};
    /* Budget state measured before any recording. */
    OutboundBudgetStatus budget{};
    /* How the control gate was satisfied; populated for every outcome
     * that reached the gate (executed and dry-run). */
    OutboundAuthorization authorization{};
};

/* Optional #141 standing-authorization input to the control gate. When
 * present, the gate passes when the digest is approved OR the envelope
 * covers the action. When absent, behaviour is exactly the pre-#141
 * per-digest gate. The coverage is computed by the caller from envelope
 * files re-read at execution time, never at decision time. */
struct OutboundEnvelopeGate {
    EnvelopeCoverage coverage;
};

/* Run the ordered gates and, only if all pass, perform the write. Deterministic
 * for fixed inputs and a fixed writer. */
[[nodiscard]] OutboundExecutionResult
execute_outbound_action(const ControlState &control, const OutboundPolicy &policy,
                        OutboundBudgetState &budget, const OutboundAction &action,
                        const OutboundWriterFactory &writer_for, std::int64_t now,
                        const OutboundAttestationFactory &attest = {},
                        bool external_publishing_allowed = true,
                        const std::optional<OutboundEnvelopeGate> &envelope = std::nullopt);

/* One-sentence human explanation of an execution result. */
[[nodiscard]] std::string describe_outbound_execution(const OutboundExecutionResult &result);

} // namespace atperson

#endif
