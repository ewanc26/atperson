#ifndef ATPERSON_APP_CLI_ROTATION_HPP
#define ATPERSON_APP_CLI_ROTATION_HPP

// Size-based rotation of the durable learning state.
//
// A cap of 0 disables that bound. The ledger is bounded by releasing the raw
// text of its oldest observations; the model is bounded by pruning its
// least-observed vocabulary. Both are deliberate forgetting: a rebuild from a
// rotated ledger does not regrow what was released or pruned.

#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>

namespace atperson {
namespace cli {

struct SizeCaps {
    std::uint64_t ledger_max_bytes = 0u;
    std::uint64_t model_max_bytes = 0u;

    [[nodiscard]] bool enabled() const noexcept {
        return ledger_max_bytes != 0u || model_max_bytes != 0u;
    }
};

/** Reads ATPERSON_LEDGER_MAX_BYTES and ATPERSON_MODEL_MAX_BYTES (unset/0 = off). */
SizeCaps size_caps_from_env();

/**
 * Enforce `caps`: release ledger payloads first, then prune the model and
 * save it. Rewrites `model_path` only when a prune happens. The caller holds
 * the state lock.
 */
void rotate_by_size(std::ostream &out, Ledger &ledger, LanguageGraph &graph,
                    const std::filesystem::path &model_path, const SizeCaps &caps);

/**
 * Print what rotate_by_size(`caps`) would do, changing nothing: the ledger
 * payloads it would release and an estimate of the vocabulary the model would
 * be pruned to. Read-only, so it needs no state lock. The model figure is a
 * single-pass estimate; protected (valence) tokens can keep it higher.
 */
void plan_rotation(std::ostream &out, const Ledger &ledger, const LanguageGraph &graph,
                   const std::filesystem::path &model_path, const SizeCaps &caps);

} // namespace cli
} // namespace atperson

#endif
