#ifndef ATPERSON_CLI_LABEL_HPP
#define ATPERSON_CLI_LABEL_HPP

// CLI for the "bot" self-label: autonomy label [status | on | off] [--yes].
//
//   status   read the profile and report whether the `bot` self-label is set
//   on       add the label   (a public profile write)
//   off      remove it
//
// on/off only PLAN unless --yes is given: they print what would change and write
// nothing. With --yes the record is rewritten with every other field preserved
// (see autonomy/bot_label.hpp) and a compare-and-swap on the record's CID, so a
// concurrent edit is refused rather than reverted. If the profile has a labels
// field this command does not understand, nothing is written. An account with no
// profile record is never given one from scratch.
//
// The network is reached only through the injected ProfileStore, so the whole
// command is testable offline. `run_bot_label_command` is the real, Wolfram-backed
// entry point, which reads the same environment as `sync` and `publish`.

#include "atproto/profile_store.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace atperson {
namespace cli {

using ProfileStoreFactory = std::function<ProfileStore &()>;

int run_bot_label(std::ostream &out, const ProfileStoreFactory &store_for, std::string_view sub,
                  const std::vector<std::string_view> &arguments,
                  const std::filesystem::path &marker_file, std::int64_t now);

int run_bot_label_command(std::ostream &out, std::string_view sub,
                          const std::vector<std::string_view> &arguments, std::int64_t now);

} // namespace cli
} // namespace atperson

#endif
