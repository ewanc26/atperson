#include "store.hpp"

#include <cJSON.h>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <iterator>
#include <optional>
#include <string_view>

namespace atperson {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw ThoughtError("thought store: " + message);
}

/* fsync the file's directory so an appended line survives a crash after
 * the caller returns. */
void sync_parent_directory(const std::filesystem::path &path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(parent, ec);
    if (ec) {
        canonical = parent;
    }
#if defined(_WIN32)
    (void)canonical;
#else
    const int fd = ::open(canonical.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

void append_line(const std::filesystem::path &path, const std::string &line) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        throw std::runtime_error("cannot append thought store " + path.string());
    }
    file << line << '\n';
    file.flush();
    if (!file) {
        throw std::runtime_error("failed while writing thought store " + path.string());
    }
    sync_parent_directory(path);
}

std::string required_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("thought entry field '") + name + "' is missing or not a string");
    }
    return field->valuestring;
}

std::string optional_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (field == nullptr || cJSON_IsNull(field)) {
        return {};
    }
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("thought entry field '") + name + "' is present but not a string");
    }
    return field->valuestring;
}

} // namespace

bool thought_kind_is_valid(std::string_view kind) {
    return kind == "reflection";
}

std::string serialise_thought(const Thought &entry) {
    if (!thought_kind_is_valid(entry.kind)) {
        fail("unknown thought kind '" + std::string(entry.kind) + "'");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        throw std::runtime_error("failed to allocate thought entry");
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
    char *raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        throw std::runtime_error("failed to serialise thought entry");
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
        fail("entry is not a JSON object");
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
    } catch (...) {
        cJSON_Delete(root);
        throw;
    }
    cJSON_Delete(root);
    return entry;
}

void append_thought(const std::filesystem::path &path, const Thought &entry) {
    append_line(path, serialise_thought(entry));
}

ThoughtContents load_thoughts(const std::filesystem::path &path) {
    ThoughtContents contents;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return contents;
    }
    const std::string raw{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (raw.empty()) {
        return contents;
    }

    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream stream{raw};
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
    }
    /* A torn final line (crash mid-append) is truncated, not reinterpreted.
     * A complete but malformed line is corruption and fails loudly. */
    if (lines.size() == 1u && !lines.back().empty() && raw.back() != '\n') {
        contents.repaired_torn_tail = true;
        return contents;
    }
    if (!lines.empty() && !lines.back().empty() && raw.back() != '\n') {
        contents.repaired_torn_tail = true;
        lines.pop_back();
    }
    contents.thoughts.reserve(lines.size());
    for (const std::string &line : lines) {
        if (line.empty()) {
            continue;
        }
        contents.thoughts.push_back(parse_thought(line));
    }
    return contents;
}

} // namespace atperson
