#include "publish.hpp"

#include "journal/store.hpp"
#include "thought/store.hpp"

#include <cJSON.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace atperson {
namespace {

constexpr const char *kCursorFormat = "atperson-replicate-cursor";
constexpr std::uint32_t kCursorVersion = 1u;

struct JsonDeleter {
    void operator()(cJSON *json) const noexcept { cJSON_Delete(json); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

[[noreturn]] void cursor_invalid(const std::string &message) {
    throw ReplicateCursorError("replicate cursor: " + message);
}

/* Observation rkeys: TID-shaped but derived from the stable ledger id so
 * the same entry always maps to the same record key across hosts. */
std::string observation_rkey(std::uint64_t id) {
    /* 13-character base-32 lowercase, TID-alphabet, zero-padded: valid
     * record-key syntax and stable. */
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    std::string rkey(13u, 'a');
    for (int i = 12; i >= 0; --i) {
        rkey[static_cast<std::size_t>(i)] = alphabet[id & 0x1Fu];
        id >>= 5;
    }
    return rkey;
}

} // namespace

ReplicateCursor load_replicate_cursor(const std::filesystem::path &path) {
    ReplicateCursor cursor;
    std::error_code missing;
    if (!std::filesystem::exists(path, missing)) {
        return cursor;
    }
    std::ifstream input(path);
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string json = buffer.str();

    Json root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        cursor_invalid("not a JSON object at " + path.string());
    }
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(root.get(), "format");
    if (!cJSON_IsString(format) || !format->valuestring ||
        std::string(format->valuestring) != kCursorFormat) {
        cursor_invalid("unknown format at " + path.string());
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (!cJSON_IsNumber(version) || version->valuedouble != kCursorVersion) {
        cursor_invalid("unsupported version at " + path.string());
    }
    const cJSON *next_observation =
        cJSON_GetObjectItemCaseSensitive(root.get(), "next_observation_id");
    if (!cJSON_IsNumber(next_observation) || next_observation->valuedouble < 1.0) {
        cursor_invalid("missing next_observation_id at " + path.string());
    }
    cursor.next_observation_id =
        static_cast<std::uint64_t>(next_observation->valuedouble);
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root.get(), "actions_published");
    if (!cJSON_IsNumber(actions) || actions->valuedouble < 0.0) {
        cursor_invalid("missing actions_published at " + path.string());
    }
    cursor.actions_published = static_cast<std::uint64_t>(actions->valuedouble);
    const cJSON *valence = cJSON_GetObjectItemCaseSensitive(root.get(), "valence_published");
    if (!cJSON_IsNumber(valence) || valence->valuedouble < 0.0) {
        cursor_invalid("missing valence_published at " + path.string());
    }
    cursor.valence_published = static_cast<std::uint64_t>(valence->valuedouble);
    /* Absent in cursors written before thoughts existed; zero is the
     * correct backfill start. */
    const cJSON *thoughts = cJSON_GetObjectItemCaseSensitive(root.get(), "thoughts_published");
    if (cJSON_IsNumber(thoughts) && thoughts->valuedouble >= 0.0) {
        cursor.thoughts_published = static_cast<std::uint64_t>(thoughts->valuedouble);
    }
    return cursor;
}

void save_replicate_cursor(const std::filesystem::path &path,
                           const ReplicateCursor &cursor) {
    Json root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "format", kCursorFormat);
    cJSON_AddNumberToObject(root.get(), "version", kCursorVersion);
    cJSON_AddNumberToObject(root.get(), "next_observation_id",
                             static_cast<double>(cursor.next_observation_id));
    cJSON_AddNumberToObject(root.get(), "actions_published",
                             static_cast<double>(cursor.actions_published));
    cJSON_AddNumberToObject(root.get(), "valence_published",
                             static_cast<double>(cursor.valence_published));
    cJSON_AddNumberToObject(root.get(), "thoughts_published",
                             static_cast<double>(cursor.thoughts_published));

    char *raw = cJSON_PrintUnformatted(root.get());
    if (!raw) {
        throw std::runtime_error("replicate cursor: JSON printing failed");
    }
    const std::string json(raw);
    cJSON_free(raw);

    const std::filesystem::path parent = path.parent_path();
    std::error_code ignored;
    std::filesystem::create_directories(parent, ignored);

    /* Atomic save: stage then rename, so a crash never leaves a torn
     * cursor. */
    std::filesystem::path staging = path;
    staging += ".tmp";
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output << json;
    }
    std::filesystem::rename(staging, path);
}

ReplicateReport replicate_drain(const std::filesystem::path &cursor_path,
                                const std::filesystem::path &journal_path, Ledger &ledger,
                                const std::filesystem::path &thoughts_path,
                                OutboundWriter &writer, const ReplicateConfig &config) {
    ReplicateReport report;
    ReplicateCursor cursor = load_replicate_cursor(cursor_path);

    /* 1. Withdrawal propagation: recheck the bounded window of already
     * published entries for outcome changes. The ledger is the authority;
     * a withdrawn entry republishes with outcome "withdrawn" so a network
     * rebuild excludes it. */
    const std::uint64_t count = atp_ledger_count(ledger.handle());
    const std::uint64_t recheck_from = cursor.next_observation_id >
                                               static_cast<std::uint64_t>(config.withdrawal_recheck_window)
                                       ? cursor.next_observation_id -
                                             config.withdrawal_recheck_window
                                       : 1u;
    for (std::uint64_t id = recheck_from; id < cursor.next_observation_id && id <= count;
         ++id) {
        atp_ledger_entry entry{};
        if (atp_ledger_entry_at(ledger.handle(), static_cast<std::size_t>(id - 1u),
                                &entry) != ATP_OK) {
            continue;
        }
        if (entry.outcome != ATP_LEDGER_OUTCOME_WITHDRAWN) {
            continue;
        }
        ObservationRecord record;
        record.id = entry.id;
        record.source_id = entry.source_id[0] != '\0' ? entry.source_id : "";
        record.author_did = entry.author_did[0] != '\0' ? entry.author_did : "";
        record.observed_at = entry.observed_at;
        record.content_digest = entry.content_digest;
        record.schema_version = entry.schema_version;
        record.outcome = std::string(ledger_outcome_name(entry.outcome));
        const atp_conversation_context raw = ledger.entry_context(entry.id);
        record.context.reply_root_uri = raw.reply_root_uri[0] != '\0' ? raw.reply_root_uri : "";
        record.context.reply_parent_uri =
            raw.reply_parent_uri[0] != '\0' ? raw.reply_parent_uri : "";
        record.context.quote_uri = raw.quote_uri[0] != '\0' ? raw.quote_uri : "";
        try {
            writer.put_record(std::string(kObservationCollection),
                               observation_rkey(entry.id),
                               serialise_observation_record(record));
            ++report.withdrawal_updates;
        } catch (const std::exception &error) {
            report.network_failed = true;
            report.failure_detail = error.what();
            return report;
        }
    }

    /* 2. New observations: publish committed entries from the cursor
     * forward. PENDING entries are skipped — they are not yet durable
     * experience; the next drain picks them up once committed. */
    std::uint32_t written = 0u;
    for (std::uint64_t id = cursor.next_observation_id; id <= count; ++id) {
        if (written >= config.max_records_per_drain) {
            break;
        }
        atp_ledger_entry entry{};
        if (atp_ledger_entry_at(ledger.handle(), static_cast<std::size_t>(id - 1u),
                                &entry) != ATP_OK) {
            throw std::runtime_error("replicate: cannot read ledger entry " +
                                     std::to_string(id));
        }
        if (entry.outcome == ATP_LEDGER_OUTCOME_PENDING) {
            continue;
        }
        ObservationRecord record;
        record.id = entry.id;
        record.source_id = entry.source_id[0] != '\0' ? entry.source_id : "";
        record.author_did = entry.author_did[0] != '\0' ? entry.author_did : "";
        record.observed_at = entry.observed_at;
        record.content_digest = entry.content_digest;
        record.schema_version = entry.schema_version;
        record.outcome = std::string(ledger_outcome_name(entry.outcome));
        const atp_conversation_context raw = ledger.entry_context(entry.id);
        record.context.reply_root_uri = raw.reply_root_uri[0] != '\0' ? raw.reply_root_uri : "";
        record.context.reply_parent_uri =
            raw.reply_parent_uri[0] != '\0' ? raw.reply_parent_uri : "";
        record.context.quote_uri = raw.quote_uri[0] != '\0' ? raw.quote_uri : "";
        try {
            writer.put_record(std::string(kObservationCollection), observation_rkey(entry.id),
                              serialise_observation_record(record));
        } catch (const std::exception &error) {
            report.network_failed = true;
            report.failure_detail = error.what();
            save_replicate_cursor(cursor_path, cursor);
            return report;
        }
        ++written;
        ++report.observations_published;
        cursor.next_observation_id = id + 1u;
    }

    /* 3. Journal entries: actions and valence, in append order. The
     * journal loader is the validation authority; counts are the cursor.
     * Action rkeys are the frozen action ids (stable, idempotent under
     * putRecord retry); valence rkeys are TID-shaped from the entry's
     * at_epoch + ordinal so distinct applications stay distinct. */
    const JournalContents journal = load_journal(journal_path);
    for (std::uint64_t i = cursor.actions_published;
         i < static_cast<std::uint64_t>(journal.actions.size()) &&
         written < config.max_records_per_drain;
         ++i) {
        const JournalAction &action = journal.actions[static_cast<std::size_t>(i)];
        try {
            ActionRecord record;
            record.id = action.id;
            record.kind = action.kind;
            record.text = action.text;
            record.digest = action.digest;
            record.outcome = journal_action_outcome_name(action.outcome);
            record.reason = action.reason;
            record.uri = action.uri;
            record.cid = action.cid;
            record.at = action.at;
            writer.put_record(std::string(kActionCollection), action.id,
                              serialise_action_record(record));
        } catch (const std::exception &error) {
            report.network_failed = true;
            report.failure_detail = error.what();
            save_replicate_cursor(cursor_path, cursor);
            return report;
        }
        ++written;
        ++cursor.actions_published;
        ++report.actions_published;
    }
    for (std::uint64_t i = cursor.valence_published;
         i < static_cast<std::uint64_t>(journal.valence.size()) &&
         written < config.max_records_per_drain;
         ++i) {
        const JournalValence &valence = journal.valence[static_cast<std::size_t>(i)];
        try {
            ValenceRecord record;
            record.token = valence.token;
            record.kind = valence.kind;
            record.signal = valence.signal;
            record.source = valence.source;
            record.at_epoch = valence.at_epoch;
            record.at = valence.at;
            record.provenance = valence.provenance;
            writer.put_record(std::string(kValenceCollection),
                              observation_rkey(valence.at_epoch + i),
                              serialise_valence_record(record));
        } catch (const std::exception &error) {
            report.network_failed = true;
            report.failure_detail = error.what();
            save_replicate_cursor(cursor_path, cursor);
            return report;
        }
        ++written;
        ++cursor.valence_published;
        ++report.valence_published;
    }

    /* 4. Thoughts: the entity's own internal notes, published in full.
     * The rkey is the frozen thought id, so putRecord retries are
     * idempotent and reconstruct maps records back to store entries. */
    const ThoughtContents thoughts = load_thoughts(thoughts_path);
    for (std::uint64_t i = cursor.thoughts_published;
         i < static_cast<std::uint64_t>(thoughts.thoughts.size()) &&
         written < config.max_records_per_drain;
         ++i) {
        const Thought &thought = thoughts.thoughts[static_cast<std::size_t>(i)];
        try {
            ThoughtRecord record;
            record.id = thought.id;
            record.kind = thought.kind;
            record.text = thought.text;
            record.about_uri = thought.about_uri;
            record.at = thought.at;
            writer.put_record(std::string(kThoughtCollection), thought.id,
                              serialise_thought_record(record));
        } catch (const std::exception &error) {
            report.network_failed = true;
            report.failure_detail = error.what();
            save_replicate_cursor(cursor_path, cursor);
            return report;
        }
        ++written;
        ++cursor.thoughts_published;
        ++report.thoughts_published;
    }

    save_replicate_cursor(cursor_path, cursor);
    return report;
}

} // namespace atperson
