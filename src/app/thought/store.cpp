#include "store.hpp"

#include <cJSON.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>

namespace atperson {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw ThoughtError("thought store: " + message);
}

/* fsync a directory so an atomically renamed record survives a crash after
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
    static constexpr char alphabet[] = "234567abcdefghijklmnopqrstuvwxyz";
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::uint64_t micros =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now)
                                       .count());
    std::string id(13u, '2');
    for (std::size_t i = 0u; i < 13u; ++i) {
        id[12u - i] = alphabet[(micros >> (5u * static_cast<unsigned>(i))) & 0x1Fu];
    }
    return id;
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
    return dir / (std::string(id) + ".json");
}

bool thought_record_exists(const std::filesystem::path &dir, std::string_view id) {
    std::error_code ec;
    return std::filesystem::exists(thought_record_path(dir, id), ec);
}

void write_thought(const std::filesystem::path &dir, const Thought &entry) {
    if (entry.id.empty()) {
        throw std::runtime_error("thought record requires an id");
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("cannot create thought store " + dir.string() + ": " +
                                 ec.message());
    }
    const std::filesystem::path path = thought_record_path(dir, entry.id);
    if (std::filesystem::exists(path, ec)) {
        throw std::runtime_error("thought record " + path.string() + " already exists");
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    const std::string payload = serialise_thought(entry);
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create thought record " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("failed while writing thought record " + tmp.string());
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("could not commit thought record " + path.string() + ": " +
                                 rename_ec.message());
    }
    sync_parent_directory(dir);
}

ThoughtContents load_thoughts(const std::filesystem::path &dir) {
    ThoughtContents contents;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->path().extension() != ".json") {
            continue;
        }
        std::ifstream input(it->path(), std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open thought record " + it->path().string());
        }
        const std::string raw{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
        if (!input.bad() && raw.empty()) {
            continue;
        }
        Thought entry = parse_thought(raw);
        if (entry.id != it->path().stem().string()) {
            fail("record id '" + entry.id + "' does not match file " +
                 it->path().filename().string());
        }
        contents.thoughts.push_back(std::move(entry));
    }
    /* Record keys encode creation time, so id order is read (append) order. */
    std::sort(contents.thoughts.begin(), contents.thoughts.end(),
              [](const Thought &a, const Thought &b) { return a.id < b.id; });
    return contents;
}

} // namespace atperson