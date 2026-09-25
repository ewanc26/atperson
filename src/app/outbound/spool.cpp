/* Offline-safe network writes (#154): spool implementation. See
 * spool.hpp for the design contract. */
#include "spool.hpp"

#include "action.hpp"
#include "records.hpp"
#include "state.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace atperson {
namespace {

/* Zero-padded to 20 digits: lexicographic order is creation order. */
std::string seq_filename(std::int64_t seq) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%020lld.json", static_cast<long long>(seq));
    return buffer;
}

std::string read_file(const std::filesystem::path &path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("spool: cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/* Extract a JSON string value for a key; throws when missing. The spool
 * entry format is written only by serialise_spool_entry, so the parser
 * needs only the shapes that writer produces. */
std::string json_string_value(const std::string &json, const std::string &key) {
    const std::size_t key_at = json.find("\"" + key + "\":");
    if (key_at == std::string::npos) {
        throw std::runtime_error("spool: entry missing " + key);
    }
    std::size_t cursor = json.find('"', key_at + key.size() + 3);
    if (cursor == std::string::npos) {
        throw std::runtime_error("spool: entry " + key + " not a string");
    }
    std::string value;
    bool escaped = false;
    for (std::size_t index = cursor + 1; index < json.size(); ++index) {
        const char c = json[index];
        if (escaped) {
            escaped = false;
            switch (c) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': value.push_back('\r'); break;
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            default: value.push_back(c); break;
            }
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return value;
        } else {
            value.push_back(c);
        }
    }
    throw std::runtime_error("spool: entry " + key + " unterminated");
}

} // namespace

std::filesystem::path SpoolPaths::pending() const { return root / "pending"; }
std::filesystem::path SpoolPaths::denied() const { return root / "denied"; }

SpoolEntry parse_spool_entry(const std::string &json) {
    SpoolEntry entry;
    const std::size_t seq_at = json.find("\"seq\":");
    if (seq_at == std::string::npos) {
        throw std::runtime_error("spool: entry missing seq");
    }
    entry.seq = std::stoll(json.substr(seq_at + 6));
    entry.action_json = json_string_value(json, "action");
    entry.spooled_at = json_string_value(json, "spooled_at");
    return entry;
}

std::string serialise_spool_entry(const SpoolEntry &entry) {
    std::string escaped;
    for (const char c : entry.action_json) {
        switch (c) {
        case '\n': escaped += "\\n"; break;
        case '\t': escaped += "\\t"; break;
        case '\r': escaped += "\\r"; break;
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                escaped += buffer;
            } else {
                escaped.push_back(c);
            }
        }
    }
    return "{\"seq\":" + std::to_string(entry.seq) + ",\"action\":\"" + escaped +
           "\",\"spooled_at\":\"" + entry.spooled_at + "\"}";
}

SpoolStatus spool_status(const SpoolPaths &paths) {
    SpoolStatus status;
    std::error_code ec;
    for (const auto &item : std::filesystem::directory_iterator{paths.pending(), ec}) {
        if (item.is_regular_file(ec)) {
            ++status.pending_count;
            const SpoolEntry entry = parse_spool_entry(read_file(item.path()));
            status.last_seq = std::max(status.last_seq, entry.seq);
            if (!status.earliest_pending_at || entry.seq < *status.earliest_pending_at) {
                status.earliest_pending_at = entry.seq;
            }
        }
    }
    for (const auto &item : std::filesystem::directory_iterator{paths.denied(), ec}) {
        if (item.is_regular_file(ec)) {
            ++status.denied_count;
        }
    }
    return status;
}

std::vector<SpoolEntry> spool_pending(const SpoolPaths &paths) {
    std::vector<SpoolEntry> entries;
    std::error_code ec;
    for (const auto &item : std::filesystem::directory_iterator{paths.pending(), ec}) {
        if (item.is_regular_file(ec)) {
            entries.push_back(parse_spool_entry(read_file(item.path())));
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const SpoolEntry &a, const SpoolEntry &b) { return a.seq < b.seq; });
    return entries;
}

SpoolEntry spool_append(const SpoolPaths &paths, const std::string &action_json,
                        const std::string &now_rfc3339) {
    /* Validate before appending: a spool entry is always a parseable
     * action document, so the drain can trust what it reads. */
    (void)parse_outbound_action(action_json, "spool");
    std::error_code ec;
    std::filesystem::create_directories(paths.pending(), ec);
    SpoolEntry entry;
    entry.action_json = action_json;
    entry.spooled_at = now_rfc3339;
    /* Sequence: max(existing) + 1. The outbound lock serialises
     * concurrent appends. */
    for (const auto &item : std::filesystem::directory_iterator{paths.pending(), ec}) {
        if (item.is_regular_file(ec)) {
            const SpoolEntry existing = parse_spool_entry(read_file(item.path()));
            entry.seq = std::max(entry.seq, existing.seq);
        }
    }
    ++entry.seq;
    write_record(paths.pending(), seq_filename(entry.seq).substr(0, 20), serialise_spool_entry(entry));
    return entry;
}

void spool_remove(const SpoolPaths &paths, const SpoolEntry &entry) {
    std::error_code ec;
    std::filesystem::remove(paths.pending() / seq_filename(entry.seq), ec);
}

void spool_deny(const SpoolPaths &paths, const SpoolEntry &entry) {
    std::error_code ec;
    std::filesystem::create_directories(paths.denied(), ec);
    std::filesystem::rename(paths.pending() / seq_filename(entry.seq),
                            paths.denied() / seq_filename(entry.seq), ec);
}

SpoolFirstWriter::SpoolFirstWriter(SpoolPaths paths, OutboundWriter &wrapped,
                                   OutboundAction action, std::string now_rfc3339)
    : paths_(std::move(paths)), wrapped_(wrapped),
      action_json_(serialise_outbound_action(action)), now_(std::move(now_rfc3339)) {}

std::string SpoolFirstWriter::resolve_record_cid(const std::string &at_uri) {
    return wrapped_.resolve_record_cid(at_uri);
}

OutboundWriteResult SpoolFirstWriter::put_record(const std::string &collection,
                                                 const std::string &rkey,
                                                 const std::string &record_json) {
    /* Spool before the network call (#154): the record is durable locally
     * before any transport is touched. */
    const SpoolEntry entry = spool_append(paths_, action_json_, now_);
    try {
        const OutboundWriteResult result = wrapped_.put_record(collection, rkey, record_json);
        spool_remove(paths_, entry);
        return result;
    } catch (...) {
        /* Transport failure is implicit offline: the entry stays spooled
         * and the next drain retries it idempotently. */
        throw;
    }
}

SpoolDrainReport spool_drain(const SpoolPaths &paths, const OutboundAttemptPaths &attempt_paths,
                             const OutboundWriterFactory &writer_for, std::int64_t now) {
    SpoolDrainReport report;
    /* The drain manages entry lifecycle itself: the replay must not go
     * through the spool-first wrapper (the action is already spooled —
     * this entry — and a duplicate would be appended and orphaned on a
     * mid-drain failure). Clearing spool_root gives the replay exactly
     * that: a plain attempt whose outcome the drain interprets. */
    OutboundAttemptPaths replay_paths = attempt_paths;
    replay_paths.spool_root.clear();
    for (const SpoolEntry &entry : spool_pending(paths)) {
        const OutboundAction action = parse_outbound_action(entry.action_json, "spool");
        const OutboundExecutionResult result =
            attempt_outbound_action(action, replay_paths, writer_for, now);
        switch (result.outcome) {
        case OutboundExecutionOutcome::Executed:
            spool_remove(paths, entry);
            ++report.published;
            break;
        case OutboundExecutionOutcome::Denied:
            spool_deny(paths, entry);
            ++report.denied;
            break;
        case OutboundExecutionOutcome::DryRun:
        case OutboundExecutionOutcome::Deferred:
            ++report.deferred;
            break;
        case OutboundExecutionOutcome::Failed:
            ++report.failed;
            report.transport_failed = true;
            report.failure_detail = result.detail;
            return report;
        }
    }
    return report;
}

} // namespace atperson
