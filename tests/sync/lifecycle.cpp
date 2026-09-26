/* Deterministic end-to-end lifecycle harness (issue #28).
 *
 * Exercises the real durable pipeline — ledger files, graph snapshots,
 * ingestion state — across multi-run lifecycles without live network access.
 * The only fixture is the page fetcher: a scripted feed that can inject
 * pagination, transient failures, duplicates, edits and deletions.
 *
 * Scenarios run, stop, restart from disk, continue, and assert the final
 * ledger/snapshot/state. Equivalent scenario runs must produce equivalent
 * final learned state (cross-run determinism). All timestamps are fixed so
 * no wall-clock value can leak into learned state.
 *
 * The scenarios themselves live in files grouped by concern, so no one file
 * owns more than one story:
 *
 *   tests/sync/lifecycle_ingestion.cpp     catch-up, crash recovery, retry,
 *                                          dedup, withdrawal, determinism
 *   tests/sync/lifecycle_derived_state.cpp conversation context, valence
 *
 * The shared fixtures are in tests/support/lifecycle_harness.hpp. This file
 * is the driver: it owns no fixture of its own. */

#include "support/lifecycle_harness.hpp"

#include <cstdio>

int main() {
    atperson::e2e::run_ingestion_scenarios();
    atperson::e2e::run_derived_state_scenarios();

    std::printf("e2e harness passed\n");
    return 0;
}
