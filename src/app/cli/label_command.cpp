#include "label.hpp"

#include "atproto/session.hpp"
#include "atproto/wolfram_profile_store.hpp"
#include "config.hpp"

#include <memory>
#include <string>

namespace atperson {
namespace cli {

int run_bot_label_command(std::ostream &out, std::string_view sub,
                          const std::vector<std::string_view> &arguments, std::int64_t now) {
    std::unique_ptr<WolframSession> session;
    std::unique_ptr<WolframProfileStore> store;
    const ProfileStoreFactory store_for = [&]() -> ProfileStore & {
        if (!store) {
            const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
            session = std::make_unique<WolframSession>(service, required_env("ATPERSON_IDENTIFIER"),
                                                       required_env("ATPERSON_APP_PASSWORD"));
            store = std::make_unique<WolframProfileStore>(*session);
        }
        return *store;
    };
    return run_bot_label(out, store_for, sub, arguments, data_dir() / "bot-label.txt", now);
}

} // namespace cli
} // namespace atperson
