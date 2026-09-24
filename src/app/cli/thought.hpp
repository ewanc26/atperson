#ifndef ATPERSON_CLI_THOUGHT_HPP
#define ATPERSON_CLI_THOUGHT_HPP

// CLI thought commands (#142): `atperson thought <text>` records one
// self-authored internal note to the local thought store; `atperson
// thought list` prints them. Thoughts publish in full to
// click.croft.atperson.thought on the next statepub drain — they are
// the entity's own words, not third-party content.

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace atperson {
namespace cli {

/* `thought <text...>` — record one thought. Returns 0 on success. */
int run_thought_record(std::ostream &out, std::ostream &err, std::string_view text);

/* `thought list [limit]` — print thoughts in append order, newest last. */
int run_thought_list(std::ostream &out, std::ostream &err, const std::filesystem::path &data_dir,
                     std::string_view limit_arg);

} // namespace cli
} // namespace atperson

#endif
