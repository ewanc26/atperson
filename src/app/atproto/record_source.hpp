#ifndef ATPERSON_ATPROTO_RECORD_SOURCE_HPP
#define ATPERSON_ATPROTO_RECORD_SOURCE_HPP

// Wolfram-backed RecordSource (#142): the network implementation of the
// reconstruct-side record reader. Lists and fetches the entity's own
// published state records via Wolfram's typed repo wrappers, and
// re-fetches observed content from source URIs via sync getRecord —
// the reconstruct path never trusts published digests without
// re-fetching the bytes they describe.
//
// Ownership: borrows a `WolframSession`; the caller keeps it alive.
// Not thread-safe (one session, one thread).
//
// Failure modes: list_records/get_record throw std::runtime_error
// (including wolfram_error) on transport or parse failure;
// fetch_content returns nullopt when the source record is gone or
// unreachable — reconstruct treats that as fail-closed, not an error.

#include "replicate/reconstruct.hpp"
#include "session.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

class WolframRecordSource final : public RecordSource {
  public:
    explicit WolframRecordSource(WolframSession &session) noexcept : session_(session) {}

    std::vector<std::string> list_records(std::string_view collection) override;
    std::optional<std::string> get_record(std::string_view collection,
                                          std::string_view rkey) override;
    std::optional<std::string> fetch_content(std::string_view source_uri) override;

  private:
    WolframSession &session_;
};

} // namespace atperson

#endif
