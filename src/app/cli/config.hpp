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

std::string env_or(const char *name, std::string fallback = {});
std::string required_env(const char *name);

std::filesystem::path data_dir();
std::filesystem::path state_path();
std::filesystem::path ledger_path();
std::filesystem::path ingestion_state_path();
std::filesystem::path control_state_path();
std::filesystem::path outbound_policy_path();
std::filesystem::path outbound_budget_path();
std::filesystem::path outbound_audit_path();

std::vector<std::filesystem::path> durable_paths();

int parse_limit(const char *value, int fallback);

} // namespace cli
} // namespace atperson

#endif