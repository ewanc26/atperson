#include "label.hpp"

#include "autonomy/bot_label.hpp"

#include <ostream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace cli {
namespace {

[[noreturn]] void usage_error() {
    throw std::runtime_error("autonomy label usage: autonomy label [status|on|off] [--yes]");
}

} // namespace

int run_bot_label(std::ostream &out, const ProfileStoreFactory &store_for, std::string_view sub,
                  const std::vector<std::string_view> &arguments,
                  const std::filesystem::path &marker_file, std::int64_t now) {
    bool confirmed = false;
    std::string action = "status";
    bool action_given = false;
    for (const std::string_view argument : arguments) {
        if (argument == "--yes") {
            confirmed = true;
        } else if (!action_given && (argument == "status" || argument == "on" || argument == "off")) {
            action = std::string(argument);
            action_given = true;
        } else {
            usage_error();
        }
    }
    (void)sub;
    if (action == "status" && confirmed) {
        usage_error(); /* --yes confirms a write; status writes nothing */
    }

    ProfileStore &store = store_for();
    const std::optional<StoredProfile> profile = store.get();
    const std::string label(kBotLabel);

    try {
        if (action == "status") {
            const bool present = profile && profile_has_self_label(profile->json, label);
            write_label_marker(marker_file, {present, now});
            if (!profile) {
                out << "bot label: absent (this account has no profile record)\n";
            } else {
                out << "bot label: " << (present ? "present" : "absent") << '\n';
            }
            if (!present) {
                out << "automated accounts are asked to carry it: `atperson autonomy label on` "
                       "plans it, `--yes` applies it.\n";
            }
            return 0;
        }

        const bool want = action == "on";
        if (!profile) {
            if (want) {
                out << "refused: this account has no profile record, and one is not created from "
                       "scratch. Set up the profile in the Bluesky app first.\n";
                return 1;
            }
            out << "nothing to remove: this account has no profile record.\n";
            write_label_marker(marker_file, {false, now});
            return 0;
        }

        const LabelEdit edit = set_profile_self_label(profile->json, label, want);
        if (!edit.changed) {
            write_label_marker(marker_file, {want, now});
            out << "bot label already " << (want ? "present" : "absent") << "; nothing to change.\n";
            return 0;
        }
        if (!confirmed) {
            out << "plan (nothing is written): " << (want ? "add" : "remove") << " the '" << label
                << "' self-label " << (want ? "to" : "from") << " " << kProfileCollection << "/"
                << kProfileRecordKey
                << ". Every other profile field is preserved, and the write is refused if the "
                   "profile changes in the meantime.\nre-run with --yes to apply it; this is a "
                   "public change to the account's profile.\n";
            return 0;
        }
        store.put(edit.json, profile->cid);
        write_label_marker(marker_file, {want, now});
        out << "bot label " << (want ? "added to" : "removed from") << " the profile.\n";
        return 0;
    } catch (const BotLabelError &error) {
        out << "refused, nothing written: " << error.what() << '\n';
        return 1;
    }
}

} // namespace cli
} // namespace atperson
