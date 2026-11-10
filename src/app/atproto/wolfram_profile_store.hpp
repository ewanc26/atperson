#ifndef ATPERSON_ATPROTO_WOLFRAM_PROFILE_STORE_HPP
#define ATPERSON_ATPROTO_WOLFRAM_PROFILE_STORE_HPP

// Wolfram-backed ProfileStore. The only place the profile record touches the
// network: it reads with getRecord and writes back with putRecord using
// `swapRecord`, the record's CID at read time, so if anyone edited the profile in
// between (from the app, say) the write is refused rather than silently reverting
// their change.
//
// Failure modes: `std::runtime_error` (including `wolfram_error`) for any Wolfram
// call failure; a swap conflict surfaces as such a failure.

#include "profile_store.hpp"
#include "session.hpp"

namespace atperson {

class WolframProfileStore final : public ProfileStore {
  public:
    explicit WolframProfileStore(WolframSession &session) noexcept : session_(session) {}
    [[nodiscard]] std::optional<StoredProfile> get() override;
    void put(const std::string &record_json, const std::string &swap_cid) override;

  private:
    WolframSession &session_;
};

} // namespace atperson

#endif
