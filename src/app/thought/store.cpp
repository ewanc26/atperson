#include "store.hpp"

#include "state/records.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace atperson {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw ThoughtError("thought store: " + message);
}

std::string required_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("thought record field '") + name + "' is missing or not a string");
    }
    return field->valuestring;
}

std::string optional_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (field == nullptr || cJSON_IsNull(field)) {
        return {};
    }
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("thought record field '") + name + "' is present but not a string");
    }
    return field->valuestring;
}

} // namespace

bool thought_kind_is_valid(std::string_view kind) {
    return kind == "reflection" || kind == "consolidation" || kind == "movement";
}

std::string new_thought_id() {
    return new_record_id();
}

std::string serialise_thought(const Thought &entry) {
    if (!thought_kind_is_valid(entry.kind)) {
        fail("unknown thought kind '" + std::string(entry.kind) + "'");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        throw std::runtime_error("failed to allocate thought record");
    }
    cJSON_AddStringToObject(root, "format", "atperson-thought");
    cJSON_AddNumberToObject(root, "version", kThoughtFormatVersion);
    cJSON_AddStringToObject(root, "id", entry.id.c_str());
    cJSON_AddStringToObject(root, "kind", entry.kind.c_str());
    cJSON_AddStringToObject(root, "text", entry.text.c_str());
    if (!entry.about_uri.empty()) {
        cJSON_AddStringToObject(root, "about", entry.about_uri.c_str());
    } else {
        cJSON_AddNullToObject(root, "about");
    }
    cJSON_AddStringToObject(root, "at", entry.at.c_str());
    if (!entry.span_start.empty()) {
        cJSON_AddStringToObject(root, "span_start", entry.span_start.c_str());
    }
    if (!entry.span_end.empty()) {
        cJSON_AddStringToObject(root, "span_end", entry.span_end.c_str());
    }
    if (!entry.topic.empty()) {
        cJSON_AddStringToObject(root, "topic", entry.topic.c_str());
    }
    char *raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        throw std::runtime_error("failed to serialise thought record");
    }
    std::string json(raw);
    cJSON_free(raw);
    return json;
}

Thought parse_thought(std::string_view json) {
    cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        fail("record is not a JSON object");
    }
    Thought entry;
    try {
        const std::string format = required_string(root, "format");
        if (format != "atperson-thought") {
            fail("unknown format '" + format + "'");
        }
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        if (!cJSON_IsNumber(version) ||
            static_cast<std::uint32_t>(version->valuedouble) != kThoughtFormatVersion) {
            fail("unsupported version");
        }
        entry.id = required_string(root, "id");
        entry.kind = required_string(root, "kind");
        if (!thought_kind_is_valid(entry.kind)) {
            fail("unknown thought kind '" + entry.kind + "'");
        }
        entry.text = required_string(root, "text");
        entry.about_uri = optional_string(root, "about");
        entry.at = required_string(root, "at");
        entry.span_start = optional_string(root, "span_start");
        entry.span_end = optional_string(root, "span_end");
        entry.topic = optional_string(root, "topic");
    } catch (...) {
        cJSON_Delete(root);
        throw;
    }
    cJSON_Delete(root);
    return entry;
}

std::filesystem::path thought_record_path(const std::filesystem::path &dir, std::string_view id) {
    return record_path(dir, id);
}

bool thought_record_exists(const std::filesystem::path &dir, std::string_view id) {
    return record_exists(dir, id);
}

void write_thought(const std::filesystem::path &dir, const Thought &entry) {
    if (entry.id.empty()) {
        throw std::runtime_error("thought record requires an id");
    }
    write_record(dir, entry.id, serialise_thought(entry));
}

ThoughtContents load_thoughts(const std::filesystem::path &dir) {
    ThoughtContents contents;
    const std::vector<std::filesystem::path> files = list_record_files(dir);
    for (const std::filesystem::path &path : files) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open thought record " + path.string());
        }
        const std::string raw{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
        if (!input.bad() && raw.empty()) {
            continue;
        }
        Thought entry = parse_thought(raw);
        if (entry.id != path.stem().string()) {
            fail("record id '" + entry.id + "' does not match file " +
                 path.filename().string());
        }
        contents.thoughts.push_back(std::move(entry));
    }
    /* Record keys encode creation time, so id order is read (append) order. */
    std::sort(contents.thoughts.begin(), contents.thoughts.end(),
              [](const Thought &a, const Thought &b) { return a.id < b.id; });
    return contents;
}

} // namespace atperson