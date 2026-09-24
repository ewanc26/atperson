#pragma once

#include <ostream>

namespace atperson {
class LanguageGraph;
}

namespace atperson::cli {

/* Runtime-derived profile card (#152): top familiarity tokens and a stance
 * summary from valence, computed from learned state only. Near-empty on a
 * fresh graph; converges as familiarity and valence accumulate. Never
 * hand-seeded. */
int run_profile(std::ostream &out, const LanguageGraph &graph);

}
