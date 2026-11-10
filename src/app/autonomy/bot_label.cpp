#include "bot_label.hpp"

#include <cJSON.h>

#include <fstream>
#include <memory>
#include <system_error>

namespace atperson {
namespace {

constexpr std::string_view kSelfLabelsType = "com.atproto.label.defs#selfLabels";
constexpr std::size_t kMaxLabelLength = 128;

struct JsonDeleter {
    void operator()(cJSON *value) const noexcept { cJSON_Delete(value); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

[[noreturn]] void refuse(const std::string &what) {
    throw BotLabelError("profile record: " + what);
}

Json parse_profile(std::string_view json) {
    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        refuse("not a JSON object");
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root.get(), "$type");
    if (type != nullptr &&
        (!cJSON_IsString(type) || std::string_view(type->valuestring) != kProfileCollection)) {
        refuse("its $type is not app.bsky.actor.profile");
    }
    return root;
}

/* The `values` array of a well-formed selfLabels object, or nullptr when the
 * profile has no labels. Refuses any shape it does not fully understand. */
cJSON *self_label_values(cJSON *root) {
    cJSON *labels = cJSON_GetObjectItemCaseSensitive(root, "labels");
    if (labels == nullptr) {
        return nullptr;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(labels, "$type");
    if (!cJSON_IsObject(labels) || !cJSON_IsString(type) ||
        std::string_view(type->valuestring) != kSelfLabelsType) {
        refuse("'labels' is not a com.atproto.label.defs#selfLabels object; refusing to "
               "overwrite it");
    }
    cJSON *values = cJSON_GetObjectItemCaseSensitive(labels, "values");
    if (!cJSON_IsArray(values)) {
        refuse("'labels.values' is not an array; refusing to overwrite it");
    }
    for (const cJSON *entry = values->child; entry != nullptr; entry = entry->next) {
        const cJSON *val = cJSON_GetObjectItemCaseSensitive(entry, "val");
        if (!cJSON_IsObject(entry) || !cJSON_IsString(val) ||
            std::string_view(val->valuestring).size() > kMaxLabelLength) {
            refuse("'labels.values' has an entry it does not understand; refusing to overwrite it");
        }
    }
    return values;
}

bool has_value(const cJSON *values, std::string_view label) {
    if (values == nullptr) {
        return false;
    }
    for (const cJSON *entry = values->child; entry != nullptr; entry = entry->next) {
        if (std::string_view(cJSON_GetObjectItemCaseSensitive(entry, "val")->valuestring) == label) {
            return true;
        }
    }
    return false;
}

std::string print(const Json &root) {
    char *printed = cJSON_PrintUnformatted(root.get());
    if (printed == nullptr) {
        throw BotLabelError("profile record: cannot serialise");
    }
    std::string text(printed);
    cJSON_free(printed);
    return text;
}

} // namespace

std::string empty_profile_json() {
    return std::string("{\"$type\":\"") + std::string(kProfileCollection) + "\"}";
}

bool profile_has_self_label(std::string_view profile_json, std::string_view label) {
    Json root = parse_profile(profile_json);
    return has_value(self_label_values(root.get()), label);
}

LabelEdit set_profile_self_label(std::string_view profile_json, std::string_view label,
                                 bool present) {
    if (label.empty() || label.size() > kMaxLabelLength) {
        throw BotLabelError("label must be 1-128 characters");
    }
    Json root = parse_profile(profile_json);
    cJSON *values = self_label_values(root.get());
    const bool has = has_value(values, label);
    if (has == present) {
        return {std::string(profile_json), false};
    }

    if (present) {
        if (values == nullptr) {
            cJSON *labels = cJSON_AddObjectToObject(root.get(), "labels");
            cJSON_AddStringToObject(labels, "$type", std::string(kSelfLabelsType).c_str());
            values = cJSON_AddArrayToObject(labels, "values");
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "val", std::string(label).c_str());
        cJSON_AddItemToArray(values, entry);
    } else {
        for (int index = 0; index < cJSON_GetArraySize(values); ++index) {
            const cJSON *entry = cJSON_GetArrayItem(values, index);
            if (std::string_view(cJSON_GetObjectItemCaseSensitive(entry, "val")->valuestring) ==
                label) {
                cJSON_DeleteItemFromArray(values, index);
                break;
            }
        }
        if (cJSON_GetArraySize(values) == 0) {
            /* Nothing left to say: drop the field rather than leave it empty. */
            cJSON_DeleteItemFromObjectCaseSensitive(root.get(), "labels");
        }
    }
    return {print(root), true};
}

std::optional<LabelMarker> read_label_marker(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::string state;
    std::int64_t at = 0;
    if (!(input >> state >> at) || (state != "present" && state != "absent") || at < 0) {
        return std::nullopt; /* an unreadable note is simply not a confirmation */
    }
    return LabelMarker{state == "present", at};
}

void write_label_marker(const std::filesystem::path &path, const LabelMarker &marker) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        output << (marker.present ? "present " : "absent ") << marker.checked_at << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        throw std::runtime_error("cannot replace " + path.string() + ": " + ec.message());
    }
}

} // namespace atperson
