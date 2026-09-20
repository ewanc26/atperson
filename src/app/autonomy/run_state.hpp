#ifndef ATPERSON_AUTONOMY_RUN_STATE_HPP
#define ATPERSON_AUTONOMY_RUN_STATE_HPP

#include <cstdint>
#include <filesystem>
#include <string>

namespace atperson {

enum class AutonomyPhase { Bootstrap, Recovering, Learning, Proposing, AwaitingApproval,
                           Executing, Verifying, Stopped, Failed };

const char *autonomy_phase_name(AutonomyPhase phase) noexcept;
AutonomyPhase autonomy_phase_from_name(const std::string &name);

struct AutonomyRunState {
    std::uint32_t version{1};
    std::string run_id;
    AutonomyPhase phase{AutonomyPhase::Bootstrap};
    std::uint64_t checkpoint{0};
    std::string last_at;
    std::string detail;
};

AutonomyRunState load_autonomy_run_state(const std::filesystem::path &path);
void save_autonomy_run_state(const AutonomyRunState &state,
                             const std::filesystem::path &path);

} // namespace atperson

#endif
