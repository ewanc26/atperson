#ifndef ATPERSON_CLI_JETSTREAM_FILTERS_HPP
#define ATPERSON_CLI_JETSTREAM_FILTERS_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace atperson::cli {

/* Read bounded Jetstream wanted-collection NSIDs. Throws on I/O, empty input,
 * embedded whitespace, or more than 100 unique entries. */
std::vector<std::string> load_jetstream_collections(const std::filesystem::path &path);
std::vector<std::string> load_jetstream_dids(const std::filesystem::path &path);

} // namespace atperson::cli

#endif
