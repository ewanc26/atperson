#include "publish.hpp"

#include "config.hpp"
#include "attestation/proof.hpp"
#include "attestation/signer.hpp"
#include "outbound/attempt.hpp"
#include "lock.hpp"
#include "session.hpp"
#include "writer.hpp"

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <stdexcept>

#include <cstdlib>

namespace atperson {
namespace cli {
namespace {

/* The outbound lock serialises the budget read-modify-write; it is separate
 * from the daemon's long-held writer lock so publishing never blocks on, or is
 * blocked by, ingestion. */
constexpr const char *kOutboundLockName = ".outbound-lock";

} // namespace

/* The journal-integrity MAC key (#57, scoped): a 32-byte (64-hex) HMAC-SHA256
 * key. Empty or unset disables the MAC, so a run without the env var produces
 * the same journal bytes as before. The key is read from the environment only
 * when a write is actually reached — a dry run or a refusal never touches it,
 * and it is never logged or persisted. It is a symmetric journal-integrity
 * check, NOT a badge.blue record attestation and NOT a signing key. */
std::optional<atperson::JournalMac> outbound_journal_mac(
    const atperson::WolframSession &session, const atperson::OutboundAction &action) {
    const char *raw = std::getenv("ATPERSON_JOURNAL_MAC_KEY");
    if (raw == nullptr || raw[0] == '\0') {
        return std::nullopt;
    }
    return atperson::create_journal_mac(raw, session.did(), action.rkey, action.text,
                                        action.created_at);
}

int run_publish(std::ostream &out, const std::filesystem::path &data_dir,
                const std::filesystem::path &policy_file, const std::filesystem::path &budget_file,
                const std::filesystem::path &control_file, const std::filesystem::path &audit_file,
                const std::filesystem::path &journal_file,
                const std::filesystem::path &envelopes_dir,
                const std::filesystem::path &action_file, std::int64_t now) {
    const OutboundAction action = load_outbound_action(action_file);

    const StateLock outbound_lock(data_dir, kOutboundLockName);

    /* The session is established lazily: dry runs and refusals never read
     * credentials or touch the network. */
    std::unique_ptr<WolframSession> session;
    std::unique_ptr<WolframWriter> writer;
    const auto writer_for = [&]() -> OutboundWriter & {
        if (!session) {
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                       required_env("ATPERSON_APP_PASSWORD"));
            writer = std::make_unique<WolframWriter>(*session);
        }
        return *writer;
    };

    const OutboundAttestationFactory attest = [&session](const std::string &record_json,
                                                         const OutboundAction &outbound_action)
        -> std::optional<OutboundAttestation> {
        const auto signer = attestation_signer_from_environment();
        if (!signer) {
            return std::nullopt;
        }
        const std::string metadata =
            "{\"$type\":\"atperson.attestation.v1\",\"action_digest\":\"" +
            outbound_action.digest + "\",\"rkey\":\"" + outbound_action.rkey +
            "\",\"created_at\":\"" + outbound_action.created_at + "\"}";
        const auto proof = create_attestation_proof(*signer, record_json, metadata, session->did());
        return OutboundAttestation{proof.payload_cid, proof.signature_hex, proof.public_key_did,
                                   proof.key_id, proof.algorithm};
    };

    /* One attempt, one path: gate loading, execution, budget persistence,
     * audit and journal bookkeeping are shared with the autonomous
     * scheduler (#140) through the attempt atom. The CLI adds only the
     * outbound lock, the Wolfram writer and the environment-backed
     * attestation/MAC factories. */
    const OutboundAttemptPaths paths{policy_file, budget_file, control_file, audit_file,
                                    journal_file, envelopes_dir, offline_spool_path()};
    const OutboundAttemptOptions options{
        attest,
        [&session](const OutboundAction &a) -> std::optional<JournalMac> {
            if (!session) {
                return std::nullopt;
            }
            return outbound_journal_mac(*session, a);
        },
        external_publishing_enabled()};
    const OutboundExecutionResult result =
        attempt_outbound_action(action, paths, writer_for, now, options);

    out << "outcome: " << outbound_execution_outcome_name(result.outcome) << '\n'
        << "reason: " << result.reason_code << '\n'
        << "detail: " << describe_outbound_execution(result) << '\n';
    if (result.outcome == OutboundExecutionOutcome::Executed) {
        out << "uri: " << result.written.uri << '\n' << "cid: " << result.written.cid << '\n';
    }
    out << "budget recorded: " << (result.budget_recorded ? "yes" : "no") << '\n'
        << "audit: " << audit_file.string() << '\n';

    return result.outcome == OutboundExecutionOutcome::Failed ? 1 : 0;
}

} // namespace cli
} // namespace atperson
