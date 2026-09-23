#include "cli/config.hpp"

#include <cstdlib>
#include <algorithm>
#include <cctype>
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

bool external_publishing_enabled() {
    std::string value = env_or("ATPERSON_ALLOW_EXTERNAL_PUBLISHING");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value.empty() || value == "0" || value == "false" || value == "no" ||
        value == "off") {
        return false;
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    throw std::runtime_error(
        "ATPERSON_ALLOW_EXTERNAL_PUBLISHING must be one of: true, false, 1, 0, yes, no, on, off");
}

std::string self_did() {
    const std::string value = env_or("ATPERSON_SELF_DID");
    if (!value.empty() && value.rfind("did:", 0u) != 0u) {
        throw std::runtime_error(
            "ATPERSON_SELF_DID must be a DID beginning with 'did:'");
    }
    return value;
}

std::string required_self_did() {
    const std::string value = self_did();
    if (value.empty()) {
        throw std::runtime_error(
            "missing required environment variable ATPERSON_SELF_DID "
            "(Jetstream needs the entity DID to exclude self-authored records)");
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

std::filesystem::path training_dir() {
    return env_or("ATPERSON_TRAINING_HOME", (data_dir() / "training").string());
}

std::filesystem::path state_path() {
    return env_or("ATPERSON_STATE", (training_dir() / "model.bin").string());
}

std::filesystem::path ledger_path() {
    return env_or("ATPERSON_LEDGER", (training_dir() / "ledger.bin").string());
}

std::filesystem::path ingestion_state_path() {
    return env_or("ATPERSON_INGESTION_STATE",
                  (training_dir() / "ingestion-state.json").string());
}

std::filesystem::path jetstream_state_path() {
    return env_or("ATPERSON_JETSTREAM_STATE",
                  (training_dir() / "jetstream-state.json").string());
}

std::filesystem::path jetstream_collections_path() {
    const std::string configured = env_or("ATPERSON_JETSTREAM_COLLECTIONS_FILE");
    return configured.empty() ? std::filesystem::path{} :
                                std::filesystem::path(configured);
}

std::filesystem::path jetstream_dids_path() {
    const std::string configured = env_or("ATPERSON_JETSTREAM_DIDS_FILE");
    return configured.empty() ? std::filesystem::path{} :
                                std::filesystem::path(configured);
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

std::filesystem::path protocol_ledger_path() {
    return env_or("ATPERSON_PROTOCOL_LEDGER",
                  (data_dir() / "protocol-evidence.bin").string());
}

std::filesystem::path autonomy_run_state_path() {
    return env_or("ATPERSON_AUTONOMY_RUN_STATE",
                  (data_dir() / "autonomy-run.json").string());
}

std::vector<std::filesystem::path> durable_paths() {
    return {data_dir(), training_dir(), state_path(), ledger_path(), ingestion_state_path(),
            jetstream_state_path(), control_state_path(), outbound_policy_path(),
            outbound_budget_path(), outbound_audit_path(), action_journal_path(),
            protocol_ledger_path(), autonomy_run_state_path()};
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
