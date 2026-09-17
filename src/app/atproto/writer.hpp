#ifndef ATPERSON_ATPROTO_WRITER_HPP
#define ATPERSON_ATPROTO_WRITER_HPP

// Wolfram-backed outbound writer (#25): the only translation unit that turns
// an already-approved action into an AT Protocol repository write.
//
// AT Protocol mechanics belong to Wolfram (root AGENTS.md); this adapter calls
// its typed repo wrappers and never reimplements createRecord/putRecord or
// at-URI parsing. It performs no policy, approval or budget logic: those gates
// have already run in `outbound/execute.cpp`. Reply parent/root CIDs are
// resolved from the actual records here, at execution time.
//
// Ownership: borrows a `WolframSession` for its lifetime; the caller owns the
// session and must keep it alive. Not thread-safe (one session, one thread).
//
// Failure modes: `std::runtime_error` (including `wolfram_error`) for invalid
// at-URIs, missing CIDs, malformed record keys and Wolfram call failures.

#include "outbound/execute.hpp"
#include "session.hpp"

#include <string>

namespace atperson {

class WolframWriter final : public OutboundWriter {
  public:
    explicit WolframWriter(WolframSession &session) noexcept : session_(session) {}

    std::string resolve_record_cid(const std::string &at_uri) override;

    OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                   const std::string &record_json) override;

  private:
    WolframSession &session_;
};

} // namespace atperson

#endif
