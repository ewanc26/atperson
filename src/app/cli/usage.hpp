#ifndef ATPERSON_CLI_USAGE_HPP
#define ATPERSON_CLI_USAGE_HPP

// CLI usage text.
//
// Owns the human-readable command and environment reference for the
// `atperson` executable. Read-only app concern.

#include <iosfwd>

namespace atperson {
namespace cli {

void print_usage(std::ostream &out);

} // namespace cli
} // namespace atperson

#endif