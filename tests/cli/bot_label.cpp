#include "cli/bot_label.hpp"

#include <cJSON.h>

#include <cassert>
#include <string>

namespace {

cJSON *profile() {
    cJSON *value = cJSON_CreateObject();
    assert(value != nullptr);
    assert(cJSON_AddStringToObject(value, "$type", "app.bsky.actor.profile") != nullptr);
    return value;
}

} // namespace

int main() {
    cJSON *value = profile();
    assert(!atperson::cli::profile_has_bot_label(value));
    assert(atperson::cli::set_bot_self_label(value, true));
    assert(atperson::cli::profile_has_bot_label(value));

    cJSON *labels = cJSON_GetObjectItemCaseSensitive(value, "labels");
    assert(cJSON_IsObject(labels));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(labels, "$type");
    assert(cJSON_IsString(type));
    assert(std::string(type->valuestring) == "com.atproto.label.defs#selfLabels");

    assert(atperson::cli::set_bot_self_label(value, true));
    assert(atperson::cli::profile_has_bot_label(value));

    assert(atperson::cli::set_bot_self_label(value, false));
    assert(!atperson::cli::profile_has_bot_label(value));

    cJSON_Delete(value);
    return 0;
}
