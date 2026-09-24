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

/* Serialise one optional journal-integrity MAC (#57). The object is added
 * only when a payload is present, so v1 journal entries (no MAC) stay
 * byte-identical after a v1->v2 migration pass. */
void add_mac(cJSON *root, const std::optional<JournalMac> &mac) {
    if (!mac.has_value()) {
        return;
    }
    cJSON *object = cJSON_CreateObject();
    if (!object) {
        throw std::runtime_error("failed to allocate journal MAC object");
    }
    add_string(object, "mode", mac->mode);
    add_string(object, "key_hint", mac->key_hint);
    add_string(object, "sig", mac->sig);
    add_string(object, "digest", mac->digest);
    cJSON_AddItemToObject(root, "mac", object);
}

/* Serialise the optional action expectation (#149). Added only when
 * present, so v1/v2 action entries (no expectation) stay byte-identical
 * after the v2->v3 migration pass, the same pattern as the MAC field. */
void add_expectation(cJSON *root, const std::optional<JournalExpectation> &expectation) {
    if (!expectation.has_value()) {
        return;
    }
    cJSON *object = cJSON_CreateObject();
    if (!object) {
        throw std::runtime_error("failed to allocate journal expectation object");
    }
    add_string(object, "kind", expectation->kind);
    cJSON_AddNumberToObject(object, "reply_likelihood", expectation->reply_likelihood);
    cJSON *tokens = cJSON_CreateArray();
    if (!tokens) {
        cJSON_Delete(object);
        throw std::runtime_error("failed to allocate journal expectation tokens");
    }
    for (const std::string &token : expectation->tokens) {
        cJSON *item = cJSON_CreateString(token.c_str());
        if (!item) {
            cJSON_Delete(tokens);
            cJSON_Delete(object);
            throw std::runtime_error("failed to allocate journal expectation token");
        }
        cJSON_AddItemToArray(tokens, item);
    }
    cJSON_AddItemToObject(object, "tokens", tokens);
    cJSON_AddItemToObject(root, "expectation", object);
}

/* Parse the optional `expectation` object (#149). Absent or null yields
 * nullopt; a present object must carry a closed-vocabulary kind, a
 * [0, 1] reply likelihood and a capped array of non-empty token strings,
 * or the entry is rejected. */
std::optional<JournalExpectation> parse_expectation(const cJSON *root) {
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "expectation");
    if (object == nullptr || cJSON_IsNull(object)) {
        return std::nullopt;
    }
    if (!cJSON_IsObject(object)) {
        fail("journal action field 'expectation' is present but not an object");
    }
    JournalExpectation parsed;
    parsed.kind = required_string(object, "kind");
    if (parsed.kind != "action" && parsed.kind != "interaction" && parsed.kind != "approach") {
        fail("journal action expectation has unknown kind '" + parsed.kind + "'");
    }
    const cJSON *likelihood = cJSON_GetObjectItemCaseSensitive(object, "reply_likelihood");
    if (!cJSON_IsNumber(likelihood) || likelihood->valuedouble < 0.0 ||
        likelihood->valuedouble > 1.0) {
        fail("journal action expectation field 'reply_likelihood' must be a number in [0, 1]");
    }
    parsed.reply_likelihood = static_cast<float>(likelihood->valuedouble);
    const cJSON *tokens = cJSON_GetObjectItemCaseSensitive(object, "tokens");
    if (!cJSON_IsArray(tokens)) {
        fail("journal action expectation field 'tokens' is missing or not an array");
    }
    const int count = cJSON_GetArraySize(tokens);
    if (count < 0 || static_cast<std::size_t>(count) > kExpectationMaxTokens) {
        fail("journal action expectation has too many tokens");
    }
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(tokens, i);
        if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
            fail("journal action expectation 'tokens' entries must be non-empty strings");
        }
        parsed.tokens.emplace_back(item->valuestring);
    }
    return parsed;
}

/* Parse the optional `mac` object. Absent or null yields nullopt; a present
 * object must carry the four string fields or the entry is rejected. */
std::optional<JournalMac> parse_mac(const cJSON *root) {
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "mac");
    if (object == nullptr || cJSON_IsNull(object)) {
        return std::nullopt;
    }
    if (!cJSON_IsObject(object)) {
        fail("journal action field 'mac' is present but not an object");
    }
    JournalMac parsed;
    parsed.mode = optional_string(object, "mode");
    parsed.key_hint = optional_string(object, "key_hint");
    parsed.sig = optional_string(object, "sig");
    parsed.digest = optional_string(object, "digest");
    if (parsed.mode.empty() || parsed.key_hint.empty() || parsed.sig.empty() ||
        parsed.digest.empty()) {
        fail("journal action field 'mac' is missing required string fields");
    }
    return parsed;
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
    if (!cJSON_IsNumber(version)) {
        fail("journal entry has no numeric 'version' field");
    }
    const bool is_v1_action =
        version->valuedouble == 1.0 && type == "action";
    /* v2 predates the action expectation field, the resolution entry kind
     * (#149) and the intent entry kind (#150): its action/event/valence
     * lines are accepted and parse under current field semantics (absent
     * expectation == nullopt), while a v2 `resolution` or `intent` line is
     * foreign schema and refused. */
    const bool is_v2 =
        version->valuedouble == 2.0 && type != "resolution" && type != "intent";
    /* v3 adds the resolution entry kind (#149) but predates intents (#150):
     * a v3 `intent` line is foreign schema and refused. */
    const bool is_v3 = version->valuedouble == 3.0 && type != "intent";
    const bool is_current =
        version->valuedouble == static_cast<double>(kJournalFormatVersion);
    if (!is_current && !is_v3 && !is_v2 && !is_v1_action) {
        /* v1 action entries predate the journal MAC field (#57), the
         * expectation field (#149) and the intent entry kind (#150). They
         * are migrated in place: an absent MAC, an absent expectation and
         * an absent intent are the same as nullopt/absent, so the loaded
         * entry is identical to what a current writer would have produced.
         * v2 event/valence lines and v3 event/valence/resolution lines need
         * no migration. Any other version or type is refused — the journal
         * never silently reinterprets foreign schema. */
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
        action.mac = parse_mac(root.get());
        action.expectation = parse_expectation(root.get());
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
    } else if (type == "resolution") {
        JournalResolution resolution;
        resolution.action_id = required_string(root.get(), "action_id");
        resolution.state =
            journal_expectation_state_from_name(required_string(root.get(), "state"))
                .value_or(JournalExpectationState::None);
        if (resolution.state != JournalExpectationState::Met &&
            resolution.state != JournalExpectationState::Unmet &&
            resolution.state != JournalExpectationState::Expired) {
            fail("journal resolution has a non-terminal state");
        }
        resolution.at_epoch = required_u64(root.get(), "at_epoch");
        resolution.at = required_string(root.get(), "at");
        out.resolutions.push_back(std::move(resolution));
    } else if (type == "intent") {
        JournalIntent intent;
        intent.id = required_string(root.get(), "id");
        if (intent.id.empty()) {
            fail("journal intent has an empty id");
        }
        const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root.get(), "actions");
        if (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) < 1) {
            fail("journal intent field 'actions' must be a non-empty array");
        }
        const int action_count = cJSON_GetArraySize(actions);
        if (action_count > 64) {
            fail("journal intent has too many actions");
        }
        for (int i = 0; i < action_count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(actions, i);
            if (!cJSON_IsString(item) || item->valuestring == nullptr ||
                item->valuestring[0] == '\0') {
                fail("journal intent 'actions' entries must be non-empty strings");
            }
            intent.actions.emplace_back(item->valuestring);
        }
        intent.responder = required_string(root.get(), "responder");
        if (intent.responder != "anyone" &&
            intent.responder.compare(0, 4, "did:") != 0) {
            fail("journal intent responder must be 'anyone' or a specific author DID");
        }
        const std::uint64_t expires_at_epoch = required_u64(root.get(), "expires_at_epoch");
        if (expires_at_epoch == 0u) {
            fail("journal intent field 'expires_at_epoch' must be a positive number");
        }
        intent.expires_at_epoch = expires_at_epoch;
        intent.expires_at = required_string(root.get(), "expires_at");
        const std::uint64_t max_continuations = required_u64(root.get(), "max_continuations");
        if (max_continuations < 1u || max_continuations > 64u) {
            fail("journal intent field 'max_continuations' must be in [1, 64]");
        }
        intent.max_continuations = static_cast<std::uint32_t>(max_continuations);
        const auto parsed_state = intent_state_from_name(required_string(root.get(), "state"));
        if (!parsed_state.has_value()) {
            fail("journal intent has an unknown state");
        }
        intent.state = *parsed_state;
        intent.at_epoch = required_u64(root.get(), "at_epoch");
        intent.at = required_string(root.get(), "at");
        if (intent.at_epoch == 0u) {
            fail("journal intent field 'at_epoch' must be a positive number");
        }
        out.intents.push_back(std::move(intent));
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

const char *journal_expectation_state_name(JournalExpectationState state) noexcept {
    switch (state) {
    case JournalExpectationState::None:
        return "none";
    case JournalExpectationState::Pending:
        return "pending";
    case JournalExpectationState::Met:
        return "met";
    case JournalExpectationState::Unmet:
        return "unmet";
    case JournalExpectationState::Expired:
        return "expired";
    }
    return "none";
}

std::optional<JournalExpectationState>
journal_expectation_state_from_name(std::string_view name) {
    if (name == "none") {
        return JournalExpectationState::None;
    }
    if (name == "pending") {
        return JournalExpectationState::Pending;
    }
    if (name == "met") {
        return JournalExpectationState::Met;
    }
    if (name == "unmet") {
        return JournalExpectationState::Unmet;
    }
    if (name == "expired") {
        return JournalExpectationState::Expired;
    }
    return std::nullopt;
}

const char *intent_state_name(IntentState state) noexcept {
    switch (state) {
    case IntentState::Open:
        return "open";
    case IntentState::Expired:
        return "expired";
    case IntentState::Closed:
        return "closed";
    }
    return "open";
}

std::optional<IntentState> intent_state_from_name(std::string_view name) {
    if (name == "open") {
        return IntentState::Open;
    }
    if (name == "expired") {
        return IntentState::Expired;
    }
    if (name == "closed") {
        return IntentState::Closed;
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
    add_mac(root.get(), entry.mac);
    add_expectation(root.get(), entry.expectation);
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

std::string serialise_journal_resolution(const JournalResolution &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate journal resolution entry");
    }
    add_string(root.get(), "type", "resolution");
    cJSON_AddNumberToObject(root.get(), "version", kJournalFormatVersion);
    add_string(root.get(), "action_id", entry.action_id);
    add_string(root.get(), "state", journal_expectation_state_name(entry.state));
    cJSON_AddNumberToObject(root.get(), "at_epoch", static_cast<double>(entry.at_epoch));
    add_string(root.get(), "at", entry.at);
    return print_json(root.get(), "journal resolution entry");
}

std::string serialise_journal_intent(const JournalIntent &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate journal intent entry");
    }
    add_string(root.get(), "type", "intent");
    cJSON_AddNumberToObject(root.get(), "version", kJournalFormatVersion);
    add_string(root.get(), "id", entry.id);
    cJSON *actions = cJSON_CreateArray();
    if (!actions) {
        throw std::runtime_error("failed to allocate journal intent actions");
    }
    for (const std::string &action : entry.actions) {
        cJSON *item = cJSON_CreateString(action.c_str());
        if (!item) {
            cJSON_Delete(actions);
            throw std::runtime_error("failed to allocate journal intent action");
        }
        cJSON_AddItemToArray(actions, item);
    }
    cJSON_AddItemToObject(root.get(), "actions", actions);
    add_string(root.get(), "responder", entry.responder);
    cJSON_AddNumberToObject(root.get(), "expires_at_epoch",
                            static_cast<double>(entry.expires_at_epoch));
    add_string(root.get(), "expires_at", entry.expires_at);
    cJSON_AddNumberToObject(root.get(), "max_continuations",
                            static_cast<double>(entry.max_continuations));
    add_string(root.get(), "state", intent_state_name(entry.state));
    cJSON_AddNumberToObject(root.get(), "at_epoch", static_cast<double>(entry.at_epoch));
    add_string(root.get(), "at", entry.at);
    return print_json(root.get(), "journal intent entry");
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

void append_journal_resolution(const std::filesystem::path &path,
                               const JournalResolution &entry) {
    append_line(path, serialise_journal_resolution(entry));
}

void append_journal_intent(const std::filesystem::path &path, const JournalIntent &entry) {
    append_line(path, serialise_journal_intent(entry));
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
