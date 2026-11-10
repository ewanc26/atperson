#ifndef ATPERSON_ATPROTO_PROFILE_STORE_HPP
#define ATPERSON_ATPROTO_PROFILE_STORE_HPP

// Read and write the entity's own profile record (app.bsky.actor.profile/self).
//
// The interface is what the bot-label command needs and nothing more, so the
// command can be tested against a fake and this header carries no Wolfram
// dependency. The Wolfram adapter lives in wolfram_profile_store.hpp.

#include <optional>
#include <string>

namespace atperson {

struct StoredProfile {
    std::string json; /* the record value */
    std::string cid;  /* the record's CID, for compare-and-swap */
};

class ProfileStore {
  public:
    virtual ~ProfileStore() = default;
    /* nullopt when the account has no profile record. */
    [[nodiscard]] virtual std::optional<StoredProfile> get() = 0;
    /* Replace the record only if it still has CID `swap_cid`. */
    virtual void put(const std::string &record_json, const std::string &swap_cid) = 0;
};

} // namespace atperson

#endif
