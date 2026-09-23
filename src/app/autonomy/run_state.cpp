#include "autonomy/run_state.hpp"

#include <cJSON.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace atperson {
namespace {
struct JsonDelete { void operator()(cJSON *p) const noexcept { cJSON_Delete(p); } };
using Json = std::unique_ptr<cJSON, JsonDelete>;
std::string string_field(const cJSON *root, const char *name, const char *fallback = "") {
    const auto *field = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(field) && field->valuestring ? field->valuestring : fallback;
}
}

const char *autonomy_phase_name(AutonomyPhase phase) noexcept {
    switch (phase) {
    case AutonomyPhase::Bootstrap: return "bootstrap";
    case AutonomyPhase::Recovering: return "recovering";
    case AutonomyPhase::Learning: return "learning";
    case AutonomyPhase::Proposing: return "proposing";
    case AutonomyPhase::AwaitingApproval: return "awaiting_approval";
    case AutonomyPhase::Executing: return "executing";
    case AutonomyPhase::Verifying: return "verifying";
    case AutonomyPhase::Stopped: return "stopped";
    case AutonomyPhase::Failed: return "failed";
    }
    return "failed";
}

AutonomyPhase autonomy_phase_from_name(const std::string &name) {
    for (const auto phase : {AutonomyPhase::Bootstrap, AutonomyPhase::Recovering,
                             AutonomyPhase::Learning, AutonomyPhase::Proposing,
                             AutonomyPhase::AwaitingApproval, AutonomyPhase::Executing,
                             AutonomyPhase::Verifying, AutonomyPhase::Stopped,
                             AutonomyPhase::Failed}) {
        if (name == autonomy_phase_name(phase)) return phase;
    }
    throw std::runtime_error("unknown autonomy run phase: " + name);
}

AutonomyRunState load_autonomy_run_state(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Json root(cJSON_ParseWithLength(text.data(), text.size()));
    if (!root || !cJSON_IsObject(root.get())) throw std::runtime_error("invalid autonomy run state");
    const auto *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    const auto *checkpoint = cJSON_GetObjectItemCaseSensitive(root.get(), "checkpoint");
    if (!cJSON_IsNumber(version) || version->valueint != 1 || !cJSON_IsNumber(checkpoint))
        throw std::runtime_error("unsupported autonomy run state");
    AutonomyRunState state;
    state.run_id = string_field(root.get(), "run_id");
    state.phase = autonomy_phase_from_name(string_field(root.get(), "phase", "bootstrap"));
    state.checkpoint = static_cast<std::uint64_t>(checkpoint->valuedouble);
    state.last_at = string_field(root.get(), "last_at");
    state.detail = string_field(root.get(), "detail");
    return state;
}

void save_autonomy_run_state(const AutonomyRunState &state,
                             const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) throw std::runtime_error("cannot create autonomy state directory");
    Json root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON_AddStringToObject(root.get(), "run_id", state.run_id.c_str());
    cJSON_AddStringToObject(root.get(), "phase", autonomy_phase_name(state.phase));
    cJSON_AddNumberToObject(root.get(), "checkpoint", static_cast<double>(state.checkpoint));
    cJSON_AddStringToObject(root.get(), "last_at", state.last_at.c_str());
    cJSON_AddStringToObject(root.get(), "detail", state.detail.c_str());
    char *raw = cJSON_PrintUnformatted(root.get());
    if (!raw) throw std::runtime_error("cannot serialise autonomy run state");
    const std::string text(raw);
    cJSON_free(raw);
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("cannot write autonomy run state");
      output << text << '\n';
      output.flush();
      if (!output) throw std::runtime_error("cannot flush autonomy run state"); }
    std::filesystem::rename(temporary, path, ec);
    if (ec) throw std::runtime_error("cannot commit autonomy run state");
}
} // namespace atperson
