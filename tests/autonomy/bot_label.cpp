/* The "bot" self-label: the profile edit never loses another field, refuses a
 * labels field it does not understand, and the command only writes when told to,
 * with a compare-and-swap. Offline; a fake ProfileStore stands in for the PDS. */

#include "autonomy/bot_label.hpp"
#include "cli/label.hpp"

#include <cJSON.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

using namespace atperson;

const std::string kProfile =
    R"({"$type":"app.bsky.actor.profile","displayName":"Moon Bot","description":"hello",)"
    R"("avatar":{"$type":"blob","ref":{"$link":"bafyavatar"},"mimeType":"image/png","size":1234},)"
    R"("createdAt":"2026-09-17T00:00:00Z"})";

template <typename Fn> bool throws_label_error(Fn &&fn) {
    try {
        fn();
    } catch (const BotLabelError &) {
        return true;
    }
    return false;
}

cJSON *field(cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

void test_adding_the_label_preserves_every_other_field() {
    assert(!profile_has_self_label(kProfile, "bot"));
    const LabelEdit edit = set_profile_self_label(kProfile, "bot", true);
    assert(edit.changed && profile_has_self_label(edit.json, "bot"));

    cJSON *before = cJSON_Parse(kProfile.c_str());
    cJSON *after = cJSON_Parse(edit.json.c_str());
    for (const char *name : {"$type", "displayName", "description", "avatar", "createdAt"}) {
        assert(cJSON_Compare(field(before, name), field(after, name), true));
    }
    /* Exactly one new field. */
    assert(cJSON_GetArraySize(after) == cJSON_GetArraySize(before) + 1);
    cJSON *labels = field(after, "labels");
    assert(std::string(field(labels, "$type")->valuestring) == "com.atproto.label.defs#selfLabels");
    assert(cJSON_GetArraySize(field(labels, "values")) == 1);
    cJSON_Delete(before);
    cJSON_Delete(after);

    /* Idempotent: already present is not a change. */
    const LabelEdit again = set_profile_self_label(edit.json, "bot", true);
    assert(!again.changed && again.json == edit.json);
}

void test_other_labels_are_kept() {
    const std::string with_other =
        R"({"$type":"app.bsky.actor.profile","labels":{"$type":"com.atproto.label.defs#selfLabels",)"
        R"("values":[{"val":"!no-unauthenticated"}]}})";
    LabelEdit edit = set_profile_self_label(with_other, "bot", true);
    assert(edit.changed);
    assert(profile_has_self_label(edit.json, "bot"));
    assert(profile_has_self_label(edit.json, "!no-unauthenticated"));

    /* Removing bot leaves the other label, and the labels field. */
    edit = set_profile_self_label(edit.json, "bot", false);
    assert(edit.changed && !profile_has_self_label(edit.json, "bot"));
    assert(profile_has_self_label(edit.json, "!no-unauthenticated"));

    /* Removing the only label drops the field instead of leaving it empty. */
    const LabelEdit only = set_profile_self_label(
        set_profile_self_label(kProfile, "bot", true).json, "bot", false);
    assert(only.changed);
    cJSON *parsed = cJSON_Parse(only.json.c_str());
    assert(field(parsed, "labels") == nullptr && field(parsed, "displayName") != nullptr);
    cJSON_Delete(parsed);

    /* Removing something that is not there is a no-op. */
    assert(!set_profile_self_label(kProfile, "bot", false).changed);
}

void test_a_labels_field_it_does_not_understand_is_refused_not_overwritten() {
    for (const char *odd : {
             R"({"labels":{"$type":"something.else","values":[]}})",
             R"({"labels":[]})",
             R"({"labels":"bot"})",
             R"({"labels":{"$type":"com.atproto.label.defs#selfLabels","values":"bot"}})",
             R"({"labels":{"$type":"com.atproto.label.defs#selfLabels","values":[{"val":1}]}})",
             R"({"labels":{"$type":"com.atproto.label.defs#selfLabels","values":["bot"]}})"}) {
        assert(throws_label_error([&] { (void)set_profile_self_label(odd, "bot", true); }));
        assert(throws_label_error([&] { (void)profile_has_self_label(odd, "bot"); }));
    }
    /* Not a profile at all. */
    for (const char *bad : {"", "[]", "null", "{", R"({"$type":"app.bsky.feed.post"})"}) {
        assert(throws_label_error([&] { (void)set_profile_self_label(bad, "bot", true); }));
    }
    assert(throws_label_error([&] { (void)set_profile_self_label(kProfile, "", true); }));
    assert(throws_label_error([&] { (void)set_profile_self_label(kProfile, std::string(200, 'x'), true); }));
}

class FakeStore final : public ProfileStore {
  public:
    std::optional<StoredProfile> profile;
    int puts = 0;
    std::string last_put;
    std::string last_swap;
    bool fail_put = false;

    std::optional<StoredProfile> get() override { return profile; }
    void put(const std::string &json, const std::string &swap_cid) override {
        if (fail_put) {
            throw std::runtime_error("swap conflict");
        }
        ++puts;
        last_put = json;
        last_swap = swap_cid;
        profile = StoredProfile{json, swap_cid + "-next"};
    }
};

struct Run {
    int code;
    std::string output;
};

Run run(FakeStore &store, const char *sub, std::vector<std::string_view> arguments,
        const std::filesystem::path &marker) {
    std::ostringstream out;
    const cli::ProfileStoreFactory factory = [&]() -> ProfileStore & { return store; };
    const int code = cli::run_bot_label(out, factory, sub, arguments, marker, 1789639200);
    return {code, out.str()};
}

void test_the_command_plans_by_default_and_writes_only_with_yes() {
    const auto marker = std::filesystem::temp_directory_path() /
                        ("atperson-botlabel-" + std::to_string(::getpid()) + ".txt");
    std::filesystem::remove(marker);
    FakeStore store;
    store.profile = StoredProfile{kProfile, "cid-1"};

    Run result = run(store, "label", {"status"}, marker);
    assert(result.code == 0 && result.output.find("bot label: absent") != std::string::npos);
    assert(!read_label_marker(marker)->present);

    /* No --yes: a plan, and nothing is written. */
    result = run(store, "label", {"on"}, marker);
    assert(result.code == 0 && store.puts == 0);
    assert(result.output.find("plan (nothing is written)") != std::string::npos);
    assert(result.output.find("re-run with --yes") != std::string::npos);

    /* With --yes: written once, with the CID it read as the swap. */
    result = run(store, "label", {"on", "--yes"}, marker);
    assert(result.code == 0 && store.puts == 1 && store.last_swap == "cid-1");
    assert(profile_has_self_label(store.last_put, "bot"));
    assert(read_label_marker(marker)->present && read_label_marker(marker)->checked_at == 1789639200);

    /* Already on: nothing to write. */
    result = run(store, "label", {"on", "--yes"}, marker);
    assert(store.puts == 1 && result.output.find("already present") != std::string::npos);

    result = run(store, "label", {"status"}, marker);
    assert(result.output.find("bot label: present") != std::string::npos);

    /* off plans, then removes, with the newer CID. */
    result = run(store, "label", {"off"}, marker);
    assert(store.puts == 1 && result.output.find("remove") != std::string::npos);
    result = run(store, "label", {"off", "--yes"}, marker);
    assert(store.puts == 2 && store.last_swap == "cid-1-next");
    assert(!profile_has_self_label(store.last_put, "bot"));
    assert(!read_label_marker(marker)->present);
    std::filesystem::remove(marker);
}

void test_refusals_write_nothing() {
    const auto marker = std::filesystem::temp_directory_path() /
                        ("atperson-botlabel-refuse-" + std::to_string(::getpid()) + ".txt");
    std::filesystem::remove(marker);

    /* No profile record: never created from scratch. */
    FakeStore empty;
    Run result = run(empty, "label", {"on", "--yes"}, marker);
    assert(result.code == 1 && empty.puts == 0);
    assert(result.output.find("no profile record") != std::string::npos);
    result = run(empty, "label", {"status"}, marker);
    assert(result.code == 0 && result.output.find("absent") != std::string::npos);
    assert(run(empty, "label", {"off", "--yes"}, marker).code == 0);

    /* A labels field it does not understand: refused, nothing written. */
    FakeStore odd;
    odd.profile = StoredProfile{R"({"labels":{"$type":"other"}})", "cid-x"};
    result = run(odd, "label", {"on", "--yes"}, marker);
    assert(result.code == 1 && odd.puts == 0);
    assert(result.output.find("refused, nothing written") != std::string::npos);

    /* A swap conflict from the store propagates and the marker is not updated. */
    FakeStore racing;
    racing.profile = StoredProfile{kProfile, "cid-1"};
    racing.fail_put = true;
    bool threw = false;
    try {
        (void)run(racing, "label", {"on", "--yes"}, marker);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw && racing.puts == 0);

    /* Bad arguments. */
    for (const auto &bad : {std::vector<std::string_view>{"maybe"}, {"on", "off"},
                            {"status", "--yes"}, {"--nope"}}) {
        bool bad_threw = false;
        try {
            (void)run(racing, "label", bad, marker);
        } catch (const std::runtime_error &) {
            bad_threw = true;
        }
        assert(bad_threw);
    }
    std::filesystem::remove(marker);
}

void test_the_marker_is_only_a_note() {
    const auto marker = std::filesystem::temp_directory_path() /
                        ("atperson-botlabel-marker-" + std::to_string(::getpid()) + ".txt");
    std::filesystem::remove(marker);
    assert(!read_label_marker(marker).has_value());
    write_label_marker(marker, {true, 42});
    assert(read_label_marker(marker)->present && read_label_marker(marker)->checked_at == 42);
    write_label_marker(marker, {false, 43});
    assert(!read_label_marker(marker)->present);
    /* A damaged note is simply not a confirmation. */
    std::FILE *file = std::fopen(marker.c_str(), "w");
    std::fputs("garbage here\n", file);
    std::fclose(file);
    assert(!read_label_marker(marker).has_value());
    std::filesystem::remove(marker);
}

} // namespace

int main() {
    test_adding_the_label_preserves_every_other_field();
    test_other_labels_are_kept();
    test_a_labels_field_it_does_not_understand_is_refused_not_overwritten();
    test_the_command_plans_by_default_and_writes_only_with_yes();
    test_refusals_write_nothing();
    test_the_marker_is_only_a_note();
    std::puts("bot label tests passed");
    return 0;
}
