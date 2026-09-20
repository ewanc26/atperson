#ifndef ATPERSON_CLI_PROTOCOL_HPP
#define ATPERSON_CLI_PROTOCOL_HPP

#include <iosfwd>

namespace atperson::cli {
int run_protocol_resolve(std::ostream &out, std::ostream &err,
                         const char *handle, const char *service);
}

#endif
