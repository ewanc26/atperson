#include "wolfram_profile_store.hpp"

#include "autonomy/bot_label.hpp"

#include <cJSON.h>
#include <wolfram/repo_typed.h>

#include <stdexcept>

namespace atperson {

std::optional<StoredProfile> WolframProfileStore::get() {
    wf_repo_record record{};
    const wf_status status = wf_agent_get_record_typed(
        session_.agent(), session_.did().c_str(), std::string(kProfileCollection).c_str(),
        std::string(kProfileRecordKey).c_str(), nullptr, &record);
    if (status == WF_ERR_NOT_FOUND) {
        wf_repo_record_free(&record);
        return std::nullopt;
    }
    if (status != WF_OK) {
        wf_repo_record_free(&record);
        throw wolfram_error("profile getRecord", status);
    }
    if (record.value == nullptr || !record.has_cid || record.cid == nullptr) {
        wf_repo_record_free(&record);
        throw std::runtime_error("profile record came back without a value or CID");
    }
    char *printed = cJSON_PrintUnformatted(record.value);
    StoredProfile profile;
    profile.cid = record.cid;
    if (printed != nullptr) {
        profile.json = printed;
        cJSON_free(printed);
    }
    wf_repo_record_free(&record);
    if (profile.json.empty()) {
        throw std::runtime_error("profile record could not be serialised");
    }
    return profile;
}

void WolframProfileStore::put(const std::string &record_json, const std::string &swap_cid) {
    if (swap_cid.empty()) {
        throw std::runtime_error("refusing to write the profile without a swap CID");
    }
    wf_repo_write_record_result result{};
    const wf_status status = wf_agent_put_record_typed(
        session_.agent(), session_.did().c_str(), std::string(kProfileCollection).c_str(),
        std::string(kProfileRecordKey).c_str(), /*validate=*/1, record_json.c_str(),
        swap_cid.c_str(), nullptr, &result);
    wf_repo_write_record_result_free(&result);
    if (status != WF_OK) {
        throw wolfram_error("profile putRecord", status);
    }
}

} // namespace atperson
