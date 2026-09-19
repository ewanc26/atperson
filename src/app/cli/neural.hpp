#ifndef ATPERSON_CLI_NEURAL_HPP
#define ATPERSON_CLI_NEURAL_HPP

#include "runtime.hpp"

#include <filesystem>
#include <iosfwd>
#include <vector>

namespace atperson {
namespace cli {

/*
 * Explicit operator mutation for issue #66. The command acquires the state
 * writer lock before loading a candidate, binds the migration to the durable
 * ledger boundary, applies the C23 migration transactionally, and persists
 * the candidate through the snapshot writer's atomic replacement path.
 */
int run_neural_expand(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    const std::filesystem::path &data_dir,
    const std::filesystem::path &ledger_file,
    const std::filesystem::path &model_path,
    const std::vector<std::filesystem::path> &resource_paths,
    const ResourceOverrides &resource_overrides);

} // namespace cli
} // namespace atperson

#endif
