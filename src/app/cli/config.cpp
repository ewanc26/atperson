#include "cli/config.hpp"

#include <cstdlib>
#include <stdexcept>

namespace atperson {
namespace cli {

std::string env_or(const char *name, std::string fallback) {
    if (const char *value = std::getenv(name); value && value[0] != '\0') {
        return value;
    }
    return fallback;
}

std::string required_env(const char *name) {
    const std::string value = env_or(name);
    if (value.empty()) {
        throw std::runtime_error(std::string("missing required environment variable ") + name);
    }
    return value;
}

std::filesystem::path data_dir() {
    if (const std::string home_data = env_or("ATPERSON_HOME"); !home_data.empty()) {
        return home_data;
    }
    const char *home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".ewanc26" / "atperson";
    }
    return ".atperson";
}

std::filesystem::path state_path() {
    return env_or("ATPERSON_STATE", (data_dir() / "model.bin").string());
}

std::filesystem::path ledger_path() {
    return env_or("ATPERSON_LEDGER", (data_dir() / "ledger.bin").string());
}

std::filesystem::path ingestion_state_path() {
    return env_or("ATPERSON_INGESTION_STATE",
                  (data_dir() / "ingestion-state.json").string());
}

std::filesystem::path control_state_path() {
    return env_or("ATPERSON_CONTROL_STATE",
                  (data_dir() / "control-state.json").string());
}

std::filesystem::path outbound_policy_path() {
    return env_or("ATPERSON_OUTBOUND_POLICY",
                  (data_dir() / "outbound-policy.json").string());
}

std::filesystem::path outbound_budget_path() {
    return env_or("ATPERSON_OUTBOUND_BUDGET",
                  (data_dir() / "outbound-budget.json").string());
}

std::filesystem::path outbound_audit_path() {
    return env_or("ATPERSON_OUTBOUND_AUDIT",
                  (data_dir() / "outbound-audit.jsonl").string());
}

std::filesystem::path action_journal_path() {
    return env_or("ATPERSON_ACTION_JOURNAL",
                  (data_dir() / "action-journal.jsonl").string());
}

std::vector<std::filesystem::path> durable_paths() {
    return {data_dir(), state_path(), ledger_path(), ingestion_state_path(),
            control_state_path(), outbound_policy_path(), outbound_budget_path(),
            outbound_audit_path(), action_journal_path()};
}

int parse_limit(const char *value, int fallback) {
    if (!value) {
        return fallback;
    }
    const int parsed = std::stoi(value);
    if (parsed <= 0) {
        throw std::runtime_error("limit must be positive");
    }
    return parsed;
}

} // namespace cli
} // namespace atperson