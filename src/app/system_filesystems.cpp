/*
 * Multi-filesystem resource probing.
 *
 * CPU and memory are process-wide, so probe them once through the platform
 * implementation in system_resources.cpp. Durable storage may be split by
 * ATPERSON_STATE / ATPERSON_LEDGER / ATPERSON_INGESTION_STATE; retain each
 * filesystem reading so budget derivation can select the tightest headroom.
 */

#include "system_resources.hpp"

#include <system_error>

namespace atperson {

SystemResources probe_system_resources(
    const std::vector<std::filesystem::path> &data_paths) {
    if (data_paths.empty()) {
        return probe_system_resources(std::filesystem::current_path());
    }

    SystemResources combined = probe_system_resources(data_paths.front());
    combined.filesystems.clear();

    for (const auto &path : data_paths) {
        const SystemResources single = probe_system_resources(path);
        combined.filesystems.push_back(FilesystemResources{
            .path = path,
            .capacity_bytes = single.disk_capacity_bytes,
            .available_bytes = single.disk_available_bytes,
        });
    }

    /* Keep the legacy summary conservative for code that has not yet switched
     * to the full vector: select the destination with least absolute free
     * space. Resource-budget derivation uses every entry and is stricter. */
    if (!combined.filesystems.empty()) {
        const FilesystemResources *limiting = &combined.filesystems.front();
        for (const auto &filesystem : combined.filesystems) {
            if (filesystem.available_bytes < limiting->available_bytes) {
                limiting = &filesystem;
            }
        }
        combined.disk_capacity_bytes = limiting->capacity_bytes;
        combined.disk_available_bytes = limiting->available_bytes;
    }
    return combined;
}

} // namespace atperson
