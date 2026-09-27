#include "publication.hpp"

#include "file_writer.hpp"

#include "control/state.hpp"
#include "resource/runtime.hpp"
#include "state/lock.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace atperson {
namespace {

std::uint32_t parse_bounded_u32(const char *value, std::uint32_t fallback,
                                std::uint32_t maximum) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ull) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::min<unsigned long long>(parsed, maximum));
}

} // namespace

std::filesystem::path replicate_cursor_path(const std::filesystem::path &data_dir) {
    return data_dir / "replicate" / "cursor.json";
}

std::filesystem::path replicate_records_dir(const std::filesystem::path &data_dir) {
    return data_dir / "replicate" / "records";
}

PublicationConfig publication_config_from_environment() {
    PublicationConfig config;
    const char *enabled = std::getenv("ATPERSON_STATEPUB");
    config.enabled = enabled != nullptr && std::string_view(enabled) == "1";
    config.max_records_per_drain = parse_bounded_u32(
        std::getenv("ATPERSON_STATEPUB_MAX_RECORDS"), config.max_records_per_drain, 5000u);
    config.withdrawal_recheck_window = parse_bounded_u32(
        std::getenv("ATPERSON_STATEPUB_RECHECK_WINDOW"), config.withdrawal_recheck_window, 100000u);
    config.every_cycles =
        parse_bounded_u32(std::getenv("ATPERSON_STATEPUB_CYCLES"), config.every_cycles, 100000u);
    return config;
}

bool publication_due(const PublicationConfig &config, std::uint64_t cycle) noexcept {
    if (!config.enabled || cycle == 0u) {
        return false;
    }
    const std::uint32_t every = config.every_cycles == 0u ? 1u : config.every_cycles;
    return cycle % every == 0u;
}

PublicationReport run_publication_pass(const PublicationPaths &paths,
                                       const PublicationConfig &config,
                                       const RuntimeResourceStatus &resource_status,
                                       Ledger &ledger, bool offline,
                                       const OnlineWriterFactory &online_writer) {
    PublicationReport report;

    /* Disk headroom first: the pass's own writes are the thing that runs
     * the disk down, so a host that is already tight must not be told to
     * write more. The backlog stays local and the next pass retries. */
    try {
        require_runtime_write_headroom(resource_status);
    } catch (const std::exception &error) {
        report.refused = true;
        report.refusal = error.what();
        return report;
    }

    /* Fail-closed control gate: publication is an outbound network write
     * procedure, so the master write gate applies exactly as it does to
     * outbound actions. An unreadable control file refuses too — it must
     * never be read as "no gate configured, therefore allowed". */
    try {
        if (!load_control_state(paths.control_file).writes_enabled) {
            report.refused = true;
            report.refusal = "writes are disabled in control state";
            return report;
        }
    } catch (const std::exception &error) {
        report.refused = true;
        report.refusal = std::string("control state unreadable: ") + error.what();
        return report;
    }

    /* The publication lock is separate from the daemon's writer lock, so a
     * pass and a drain can never interleave on one cursor. Failing to take
     * it means another pass is running: skip this cycle. */
    try {
        const StateLock lock(paths.data_dir, kStatepubLockName);
        FileWriter staged(paths.records_dir);
        ReplicateConfig drain_config;
        drain_config.max_records_per_drain = config.max_records_per_drain;
        drain_config.withdrawal_recheck_window = config.withdrawal_recheck_window;
        report.ran = true;
        report.staged_offline = offline;
        report.drain = replicate_drain(paths.cursor_file, paths.journal_file, ledger,
                                       paths.thoughts_dir,
                                       offline ? static_cast<OutboundWriter &>(staged)
                                               : online_writer(),
                                       drain_config);
    } catch (const std::exception &error) {
        /* A transport failure, a busy lock, a corrupt cursor: the cursor
         * stays at the last confirmed write, so the next pass resumes here. */
        report.refused = true;
        report.refusal = error.what();
    }
    return report;
}

} // namespace atperson
