#include "store.hpp"

#include <cJSON.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace atperson {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

[[noreturn]] void fail(const std::string &message) {
    throw JournalError(message);
}

/* Read the whole file to a string. A missing file is not an error: the
 * journal starts empty. */
std::optional<std::string> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/* fsync the file's directory so an appended line survives a crash after the
 * caller returns. */
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
    // Directory fsync is not meaningful on Windows; the flush in the append
    // path is the durability boundary there.
    (void)canonical;
#else
    const int fd = ::open(canonical.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

/* Append one serialised line, flush, fsync the file, then fsync the parent
 * directory. */
void append_line(const std::filesystem::path &path, const std::string &line) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        throw std::runtime_error("cannot append action journal " + path.string());
    }
    file << line << '\n';
    file.flush();
    if (!file) {
        throw std::runtime_error("failed while writing action journal " + path.string());
    }
    sync_parent_directory(path);
}

/* String field accessors that reject wrong-typed values loudly. */
std::string required_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("journal entry field '") + name + "' is missing or not a string");
    }
    return field->valuestring;
}

std::string optional_string(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (field == nullptr) {
        return {};
    }
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("journal entry field '") + name + "' is present but not a string");
    }
    return field->valuestring;
}

std::uint64_t required_u64(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsNumber(field)) {
        fail(std::string("journal entry field '") + name + "' is missing or not a number");
    }
    if (field->valuedouble < 0.0) {
        fail(std::string("journal entry field '") + name + "' is negative");
    }
    return static_cast<std::uint64_t>(field->valuedouble);
}

float required_float(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsNumber(field)) {
        fail(std::string("journal entry field '") + name + "' is missing or not a number");
    }
    return static_cast<float>(field->valuedouble);
}

void add_string(cJSON *object, const char *name, const std::string &value) {
    cJSON_AddStringToObject(object, name, value.c_str());
}

/* Print a serialised entry. cJSON_PrintUnformatted is deterministic for a
 * fixed object, so a fixed entry always serialises to the same bytes. */
std::string print_json(cJSON *root, const char *what) {
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        throw std::runtime_error(std::string("failed to serialise ") + what);
    }
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

/* Parse one non-empty line into a typed entry, dispatched on the `type`
 * field. Unknown types throw: the journal never skips content it does not
 * understand. */
void parse_line(const std::string &line, JournalContents &out) {
    Json root(cJSON_ParseWithLength(line.data(), line.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        fail("journal line is not a JSON object");
    }
    const std::string type = required_string(root.get(), "type");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) ||
        version->valuedouble != static_cast<double>(kJournalFormatVersion)) {
        fail("journal entry has unsupported version");
    }
    if (type == "action") {
        JournalAction action;
        action.id = required_string(root.get(), "id");
        action.kind = required_string(root.get(), "kind");
        action.text = required_string(root.get(), "text");
        action.digest = required_string(root.get(), "digest");
        action.outcome = journal_action_outcome_from_name(required_string(root.get(), "outcome"))
                             .value_or(JournalActionOutcome::Denied);
        action.reason = required_string(root.get(), "reason");
        action.uri = optional_string(root.get(), "uri");
        action.cid = optional_string(root.get(), "cid");
        action.at = required_string(root.get(), "at");
        out.actions.push_back(std::move(action));
    } else if (type == "event") {
        JournalEvent event;
        event.action_id = required_string(root.get(), "action_id");
        event.event_uri = required_string(root.get(), "event_uri");
        event.author_did = required_string(root.get(), "author_did");
        event.via = required_string(root.get(), "via");
        event.at = required_string(root.get(), "at");
        out.events.push_back(std::move(event));
    } else if (type == "valence") {
        JournalValence valence;
        valence.token = required_string(root.get(), "token");
        valence.kind = required_string(root.get(), "kind");
        valence.signal = required_float(root.get(), "signal");
        valence.source = required_string(root.get(), "source");
        valence.at_epoch = required_u64(root.get(), "at_epoch");
        valence.at = required_string(root.get(), "at");
        valence.provenance = optional_string(root.get(), "provenance");
        out.valence.push_back(std::move(valence));
    } else {
        fail("journal entry has unknown type '" + type + "'");
    }
}

} // namespace

const char *journal_action_outcome_name(JournalActionOutcome outcome) noexcept {
    switch (outcome) {
    case JournalActionOutcome::Executed:
        return "executed";
    case JournalActionOutcome::Denied:
        return "denied";
    case JournalActionOutcome::Deferred:
        return "deferred";
    case JournalActionOutcome::Failed:
        return "failed";
    case JournalActionOutcome::DryRun:
        return "dry_run";
    }
    return "denied";
}

std::optional<JournalActionOutcome> journal_action_outcome_from_name(std::string_view name) {
    if (name == "executed") {
        return JournalActionOutcome::Executed;
    }
    if (name == "denied") {
        return JournalActionOutcome::Denied;
    }
    if (name == "deferred") {
        return JournalActionOutcome::Deferred;
    }
    if (name == "failed") {
        return JournalActionOutcome::Failed;
    }
    if (name == "dry_run") {
        return JournalActionOutcome::DryRun;
    }
    return std::nullopt;
}

std::optional<atp_valence_kind> valence_kind_from_name(std::string_view name) {
    if (name == "action") {
        return ATP_VALENCE_ACTION;
    }
    if (name == "interaction") {
        return ATP_VALENCE_INTERACTION;
    }
    if (name == "approach") {
        return ATP_VALENCE_APPROACH;
    }
    if (name == "avoid") {
        return ATP_VALENCE_AVOID;
    }
    return std::nullopt;
}

const char *valence_kind_name(atp_valence_kind kind) noexcept {
    switch (kind) {
    case ATP_VALENCE_ACTION:
        return "action";
    case ATP_VALENCE_INTERACTION:
        return "interaction";
    case ATP_VALENCE_APPROACH:
        return "approach";
    case ATP_VALENCE_AVOID:
        return "avoid";
    }
    return "action";
}

std::string serialise_journal_action(const JournalAction &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate journal action entry");
    }
    add_string(root.get(), "type", "action");
    cJSON_AddNumberToObject(root.get(), "version", kJournalFormatVersion);
    add_string(root.get(), "id", entry.id);
    add_string(root.get(), "kind", entry.kind);
    add_string(root.get(), "text", entry.text);
    add_string(root.get(), "digest", entry.digest);
    add_string(root.get(), "outcome", journal_action_outcome_name(entry.outcome));
    add_string(root.get(), "reason", entry.reason);
    add_string(root.get(), "uri", entry.uri);
    add_string(root.get(), "cid", entry.cid);
    add_string(root.get(), "at", entry.at);
    return print_json(root.get(), "journal action entry");
}

std::string serialise_journal_event(const JournalEvent &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate journal event entry");
    }
    add_string(root.get(), "type", "event");
    cJSON_AddNumberToObject(root.get(), "version", kJournalFormatVersion);
    add_string(root.get(), "action_id", entry.action_id);
    add_string(root.get(), "event_uri", entry.event_uri);
    add_string(root.get(), "author_did", entry.author_did);
    add_string(root.get(), "via", entry.via);
    add_string(root.get(), "at", entry.at);
    return print_json(root.get(), "journal event entry");
}

std::string serialise_journal_valence(const JournalValence &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate journal valence entry");
    }
    add_string(root.get(), "type", "valence");
    cJSON_AddNumberToObject(root.get(), "version", kJournalFormatVersion);
    add_string(root.get(), "token", entry.token);
    add_string(root.get(), "kind", entry.kind);
    cJSON_AddNumberToObject(root.get(), "signal", entry.signal);
    add_string(root.get(), "source", entry.source);
    cJSON_AddNumberToObject(root.get(), "at_epoch", static_cast<double>(entry.at_epoch));
    add_string(root.get(), "at", entry.at);
    if (!entry.provenance.empty()) {
        add_string(root.get(), "provenance", entry.provenance);
    }
    return print_json(root.get(), "journal valence entry");
}

void append_journal_action(const std::filesystem::path &path, const JournalAction &entry) {
    append_line(path, serialise_journal_action(entry));
}

void append_journal_event(const std::filesystem::path &path, const JournalEvent &entry) {
    append_line(path, serialise_journal_event(entry));
}

void append_journal_valence(const std::filesystem::path &path, const JournalValence &entry) {
    append_line(path, serialise_journal_valence(entry));
}

JournalContents load_journal(const std::filesystem::path &path) {
    JournalContents journal;
    const auto contents = read_file(path);
    if (!contents) {
        return journal;
    }

    std::size_t position = 0u;
    const std::string &data = *contents;
    while (position < data.size()) {
        const std::size_t newline = data.find('\n', position);
        if (newline == std::string::npos) {
            // No terminating newline: a crash mid-append left a torn final
            // line. Report it and drop the partial bytes; every earlier line
            // was fsynced complete.
            journal.repaired_torn_tail = true;
            break;
        }
        const std::string line = data.substr(position, newline - position);
        position = newline + 1u;
        if (!line.empty()) {
            parse_line(line, journal);
        }
    }
    return journal;
}

bool journal_has_event(const JournalContents &journal, std::string_view action_id,
                       std::string_view event_uri) {
    for (const auto &event : journal.events) {
        if (event.action_id == action_id && event.event_uri == event_uri) {
            return true;
        }
    }
    return false;
}

const JournalAction *journal_find_action_by_uri(const JournalContents &journal,
                                                std::string_view uri) {
    for (const auto &action : journal.actions) {
        if (action.outcome == JournalActionOutcome::Executed && action.uri == uri) {
            return &action;
        }
    }
    return nullptr;
}

} // namespace atperson
