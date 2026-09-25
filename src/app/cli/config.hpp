#ifndef ATPERSON_CLI_CONFIG_HPP
#define ATPERSON_CLI_CONFIG_HPP

// CLI configuration: environment and durable-path resolution.
//
// Owns how the `atperson` CLI discovers its runtime environment: the data
// directory override, the model snapshot, ledger and ingestion-state paths,
// and bounded integer option parsing. Pure app concern; no network, no core
// mutation. Throws std::runtime_error when a required environment variable is
// missing; path helpers fall back to defaults per the documented environment.

#include <filesystem>
#include <string>
#include <vector>

namespace atperson {
namespace cli {

/* Canonical public Jetstream endpoint (#60). The subscribeEvents RPC enables
 * v2 sequence cursors and is required for archive-to-live cutover; the legacy
 * /subscribe endpoint is for explicitly configured live-only consumers. */
inline constexpr const char *kDefaultJetstreamEndpoint =
    "wss://jetstream.us-east.bsky.network/xrpc/network.bsky.jetstream.subscribeEvents";

std::string env_or(const char *name, std::string fallback = {});
std::string required_env(const char *name);
bool external_publishing_enabled();

/* Non-secret repository identity used by unauthenticated public ingestion to
 * enforce the self-authored exclusion from #20. Empty means unconfigured. */
std::string self_did();
std::string required_self_did();

std::filesystem::path data_dir();
std::filesystem::path training_dir();
std::filesystem::path state_path();
std::filesystem::path ledger_path();
std::filesystem::path ingestion_state_path();

/* Independent unauthenticated Jetstream cursor/checkpoint (#60). Keeping it
 * separate from the authenticated timeline state lets both ingestion paths
 * resume across restarts without invalidating each other's source identity. */
std::filesystem::path jetstream_state_path();

/* Optional operator-owned one-collection-filter-per-line file. Empty path
 * means the default public-post collection. */
std::filesystem::path jetstream_collections_path();
std::filesystem::path jetstream_dids_path();
std::filesystem::path jetstream_kinds_path();

std::filesystem::path control_state_path();
std::filesystem::path outbound_policy_path();
std::filesystem::path outbound_budget_path();
std::filesystem::path outbound_audit_path();

/* Action/outcome journal (#27): durable experience provenance for the
 * entity's own outbound attempts. */
std::filesystem::path action_journal_path();
std::filesystem::path protocol_ledger_path();
std::filesystem::path autonomy_run_state_path();
std::filesystem::path autonomy_heartbeat_path();

/* Autonomous scheduler (#140): frozen proposal documents awaiting operator
 * approval or execution. */
std::filesystem::path scheduler_proposals_path();
std::filesystem::path authorization_envelopes_path();

std::vector<std::filesystem::path> durable_paths();

int parse_limit(const char *value, int fallback);

} // namespace cli
} // namespace atperson

#endif
