#include "cli/bot_label.hpp"

#include "config.hpp"
#include "session.hpp"

#include <wolfram/repo_typed.h>

#include <memory>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace atperson::cli {
namespace {

constexpr const char *kProfileCollection = "app.bsky.actor.profile";
constexpr const char *kProfileRkey = "self";
constexpr const char *kSelfLabelsType = "com.atproto.label.defs#selfLabels";
constexpr const char *kBotValue = "bot";

bool is_bot_value(const cJSON *value) {
    if (!cJSON_IsObject(value)) {
        return false;
    }
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(value, "val");
    return cJSON_IsString(val) && val->valuestring != nullptr &&
           std::string(val->valuestring) == kBotValue;
}

} // namespace

bool profile_has_bot_label(const cJSON *profile) {
    if (!cJSON_IsObject(profile)) {
        return false;
    }

    const cJSON *labels = cJSON_GetObjectItemCaseSensitive(profile, "labels");
    if (!cJSON_IsObject(labels)) {
        return false;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(labels, "$type");
    if (!cJSON_IsString(type) || type->valuestring == nullptr ||
        std::string(type->valuestring) != kSelfLabelsType) {
        return false;
    }

    const cJSON *values = cJSON_GetObjectItemCaseSensitive(labels, "values");
    if (!cJSON_IsArray(values)) {
        return false;
    }

    const cJSON *value = nullptr;
    cJSON_ArrayForEach(value, values) {
        if (is_bot_value(value)) {
            return true;
        }
    }
    return false;
}

bool set_bot_self_label(cJSON *profile, bool enabled) {
    if (!cJSON_IsObject(profile)) {
        return false;
    }

    if (!enabled) {
        if (!cJSON_HasObjectItem(profile, "labels")) {
            return true;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(profile, "labels");
        return true;
    }

    cJSON *labels = cJSON_CreateObject();
    cJSON *values = cJSON_CreateArray();
    cJSON *bot = cJSON_CreateObject();
    if (labels == nullptr || values == nullptr || bot == nullptr) {
        cJSON_Delete(labels);
        cJSON_Delete(values);
        cJSON_Delete(bot);
        return false;
    }

    if (!cJSON_AddStringToObject(labels, "$type", kSelfLabelsType) ||
        !cJSON_AddStringToObject(bot, "val", kBotValue)) {
        cJSON_Delete(labels);
        cJSON_Delete(values);
        cJSON_Delete(bot);
        return false;
    }
    cJSON_AddItemToArray(values, bot);
    cJSON_AddItemToObject(labels, "values", values);

    cJSON_ReplaceItemInObjectCaseSensitive(profile, "labels", labels);
    return true;
}

int run_bot_label(std::ostream &out, std::ostream &err, const char *subcommand) {
    const std::string command = subcommand == nullptr ? "status" : subcommand;
    if (command != "status" && command != "set" && command != "clear") {
        err << "autonomy bot-label: expected status, set or clear\n";
        return 2;
    }

    try {
        const std::string service = env_or("ATPERSON_SERVICE", "https://bsky.social");
        WolframSession session(service, required_env("ATPERSON_IDENTIFIER"),
                               required_env("ATPERSON_APP_PASSWORD"));

        wf_repo_record record{};
        const wf_status status = wf_agent_get_record_typed(
            session.agent(), session.did().c_str(), kProfileCollection, kProfileRkey,
            nullptr, &record);

        if (status != WF_OK) {
            wf_repo_record_free(&record);
            if (command == "status") {
                err << "autonomy bot-label: unable to read profile: "
                    << static_cast<int>(status) << '\n';
                return 1;
            }

            if (command == "clear") {
                out << "bot self-label: already clear\\n";
                return 0;
            }

            cJSON *profile = cJSON_CreateObject();
            if (profile == nullptr) {
                throw std::runtime_error("unable to allocate profile record");
            }
            cJSON_AddStringToObject(profile, "$type", kProfileCollection);
            if (!set_bot_self_label(profile, true)) {
                cJSON_Delete(profile);
                throw std::runtime_error("unable to add bot self-label");
            }

            char *json = cJSON_PrintUnformatted(profile);
            cJSON_Delete(profile);
            if (json == nullptr) {
                throw std::runtime_error("unable to serialise profile record");
            }

            wf_repo_write_record_result result{};
            const wf_status write_status = wf_agent_create_record_typed(
                session.agent(), session.did().c_str(), kProfileCollection, kProfileRkey,
                1, json, nullptr, &result);
            std::free(json);
            if (write_status != WF_OK) {
                wf_repo_write_record_result_free(&result);
                throw wolfram_error("bot self-label putRecord", write_status);
            }
            wf_repo_write_record_result_free(&result);
            out << "bot self-label: set\n";
            return 0;
        }

        if (!record.value || !cJSON_IsObject(record.value)) {
            wf_repo_record_free(&record);
            throw std::runtime_error("profile record has no object value");
        }

        const bool already = profile_has_bot_label(record.value);
        if (command == "status") {
            out << "bot self-label: " << (already ? "set" : "not set") << '\n';
            wf_repo_record_free(&record);
            return already ? 0 : 1;
        }

        const bool enabled = command == "set";
        if (already == enabled) {
            out << "bot self-label: " << (enabled ? "already set" : "already clear") << '\n';
            wf_repo_record_free(&record);
            return 0;
        }

        if (!set_bot_self_label(record.value, enabled)) {
            wf_repo_record_free(&record);
            throw std::runtime_error("unable to update profile self-label");
        }

        char *json = cJSON_PrintUnformatted(record.value);
        wf_repo_record_free(&record);
        if (json == nullptr) {
            throw std::runtime_error("unable to serialise profile record");
        }

        wf_repo_write_record_result result{};
        const wf_status write_status = wf_agent_put_record_typed(
            session.agent(), session.did().c_str(), kProfileCollection, kProfileRkey,
            1, json, nullptr, nullptr, &result);
        std::free(json);
        if (write_status != WF_OK) {
            wf_repo_write_record_result_free(&result);
            throw wolfram_error("bot self-label putRecord", write_status);
        }
        wf_repo_write_record_result_free(&result);

        out << "bot self-label: " << (enabled ? "set" : "cleared") << '\n';
        return 0;
    } catch (const std::exception &error) {
        err << "autonomy bot-label: " << error.what() << '\n';
        return 1;
    }
}

} // namespace atperson::cli
