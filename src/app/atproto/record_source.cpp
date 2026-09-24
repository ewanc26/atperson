#include "record_source.hpp"

#include <wolfram/repo_typed.h>
#include <wolfram/syntax.h>
#include <wolfram/xrpc.h>

#include <cJSON.h>

#include <stdexcept>
#include <string>

namespace atperson {
namespace {

/* Extract the record-key from an at-URI (the last path segment). */
std::string uri_rkey(const std::string &at_uri) {
    const std::string::size_type slash = at_uri.rfind('/');
    if (slash == std::string::npos || slash + 1u == at_uri.size()) {
        throw std::runtime_error("record at-URI has no record key: " + at_uri);
    }
    return at_uri.substr(slash + 1u);
}

} // namespace

std::vector<std::string> WolframRecordSource::list_records(std::string_view collection) {
    std::vector<std::string> rkeys;
    std::string cursor;
    for (;;) {
        wf_repo_record_list list{};
        const wf_status status = wf_agent_list_records_typed(
            session_.agent(), session_.did().c_str(), std::string(collection).c_str(),
            /*limit=*/100, cursor.empty() ? nullptr : cursor.c_str(), /*reverse=*/1, &list);
        if (status != WF_OK) {
            wf_repo_record_list_free(&list);
            throw wolfram_error("state listRecords", status);
        }
        for (std::size_t i = 0u; i < list.count; ++i) {
            if (list.items[i].uri) {
                rkeys.push_back(uri_rkey(list.items[i].uri));
            }
        }
        const bool has_cursor = list.cursor && list.cursor[0] != '\0';
        cursor = has_cursor ? list.cursor : "";
        wf_repo_record_list_free(&list);
        if (!has_cursor) {
            break;
        }
    }
    return rkeys;
}

std::optional<std::string> WolframRecordSource::get_record(std::string_view collection,
                                                            std::string_view rkey) {
    wf_repo_record record{};
    const wf_status status = wf_agent_get_record_typed(
        session_.agent(), session_.did().c_str(), std::string(collection).c_str(),
        std::string(rkey).c_str(), /*cid_or_null=*/nullptr, &record);
    if (status != WF_OK) {
        wf_repo_record_free(&record);
        throw wolfram_error("state getRecord", status);
    }
    std::string json;
    if (record.value) {
        char *raw = cJSON_PrintUnformatted(record.value);
        if (raw) {
            json = raw;
            cJSON_free(raw);
        }
    }
    wf_repo_record_free(&record);
    if (json.empty()) {
        return std::nullopt;
    }
    return json;
}

std::optional<std::string> WolframRecordSource::fetch_content(std::string_view source_uri) {
    wf_syntax_aturi parsed{};
    if (wf_syntax_aturi_parse(std::string(source_uri).c_str(), &parsed) == 0 ||
        !parsed.authority || !parsed.collection || !parsed.record_key) {
        wf_syntax_aturi_free(&parsed);
        return std::nullopt;
    }

    wf_response response{};
    const wf_status status = wf_agent_sync_get_record(
        session_.agent(), parsed.authority, parsed.collection, parsed.record_key, &response);
    wf_syntax_aturi_free(&parsed);
    if (status != WF_OK) {
        wf_response_free(&response);
        return std::nullopt;
    }

    /* The sync response is the raw record envelope; the observation
     * payload is the record's `text` field. An empty or absent text is
     * valid content — skipped observations carry empty text and their
     * digest is the digest of zero bytes. Only an unparseable body is
     * treated as unavailable. */
    std::string content;
    cJSON *root = cJSON_ParseWithLength(response.body ? response.body : "",
                                        response.body ? response.body_len : 0u);
    if (!root) {
        wf_response_free(&response);
        return std::nullopt;
    }
    {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "record");
        if (!value) {
            value = root;
        }
        cJSON *text = cJSON_GetObjectItemCaseSensitive(value, "text");
        if (cJSON_IsString(text) && text->valuestring) {
            content = text->valuestring;
        }
        cJSON_Delete(root);
    }
    wf_response_free(&response);
    return content;
}

} // namespace atperson
