#include "selfeval/store.hpp"

#include "state/records.hpp"
#include "state/time.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

namespace atperson {
namespace {

[[noreturn]] void fail(const std::string &message) {
    throw MetricError("metric snapshot: " + message);
}

std::string required_string(const cJSON *entry, const char *name, const char *record) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsString(field) || field->valuestring == nullptr) {
        fail(std::string("'") + name + "' field of " + record + " is missing or not a string");
    }
    return field->valuestring;
}

std::uint64_t required_count(const cJSON *entry, const char *name, const char *record) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsNumber(field) || field->valuedouble < 0.0) {
        fail(std::string("'") + name + "' field of " + record + " is missing or not a count");
    }
    return static_cast<std::uint64_t>(std::llround(field->valuedouble));
}

/* Lexicons have no fractional type, so the published record encodes
 * ratios as per-mille integers (0..1000). The store record mirrors the
 * published record; scale at the JSON boundary, keep doubles in memory. */
std::int64_t ratio_to_permille(double value) {
    return static_cast<std::int64_t>(std::llround(value * 1000.0));
}

double permille_to_ratio(std::int64_t value) {
    return static_cast<double>(value) / 1000.0;
}

/* Signed milli-scaled integers for drift/sum fields: per-mille of the
 * signal scale, same round-trip treatment as ratios. */
std::int64_t signed_to_permille(double value) {
    return static_cast<std::int64_t>(std::llround(value * 1000.0));
}

double permille_to_signed(std::int64_t value) {
    return static_cast<double>(value) / 1000.0;
}

/* Reads a per-mille integer (0..1000) and returns the ratio in [0,1]. */
double required_ratio(const cJSON *entry, const char *name, const char *record) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsNumber(field) || field->valuedouble < 0.0 || field->valuedouble > 1000.0) {
        fail(std::string("'") + name + "' field of " + record + " is missing or not a ratio");
    }
    return permille_to_ratio(static_cast<std::int64_t>(std::llround(field->valuedouble)));
}

double required_signed(const cJSON *entry, const char *name, const char *record) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsNumber(field)) {
        fail(std::string("'") + name + "' field of " + record + " is missing or not a number");
    }
    return permille_to_signed(static_cast<std::int64_t>(std::llround(field->valuedouble)));
}

const cJSON *required_object(const cJSON *entry, const char *name, const char *record) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    if (!cJSON_IsObject(field)) {
        fail(std::string("'") + name + "' field of " + record + " is missing or not an object");
    }
    return field;
}

MetricActions parse_actions(const cJSON *value) {
    MetricActions actions;
    actions.attempts = required_count(value, "attempts", "actions");
    actions.executed = required_count(value, "executed", "actions");
    actions.denied = required_count(value, "denied", "actions");
    actions.deferred = required_count(value, "deferred", "actions");
    actions.failed = required_count(value, "failed", "actions");
    actions.dry_run = required_count(value, "dry_run", "actions");
    actions.success_rate = required_ratio(value, "success_rate", "actions");
    return actions;
}

MetricInteraction parse_interaction(const cJSON *value) {
    MetricInteraction interaction;
    interaction.invites = required_count(value, "invites", "interaction");
    interaction.invites_replied = required_count(value, "invites_replied", "interaction");
    interaction.invites_expired = required_count(value, "invites_expired", "interaction");
    interaction.invites_pending = required_count(value, "invites_pending", "interaction");
    interaction.success_rate = required_ratio(value, "success_rate", "interaction");
    return interaction;
}

MetricReplyRatio parse_reply_ratio(const cJSON *value) {
    MetricReplyRatio reply_ratio;
    reply_ratio.events = required_count(value, "events", "reply_ratio");
    reply_ratio.replies = required_count(value, "replies", "reply_ratio");
    reply_ratio.ratio = required_ratio(value, "ratio", "reply_ratio");
    return reply_ratio;
}

MetricValence parse_valence(const cJSON *value) {
    MetricValence valence;
    valence.updates = required_count(value, "updates", "valence");
    valence.tokens = required_count(value, "tokens", "valence");
    valence.drift = required_signed(value, "drift", "valence");
    return valence;
}

MetricFamiliarity parse_familiarity(const cJSON *value) {
    MetricFamiliarity familiarity;
    familiarity.authors = required_count(value, "authors", "familiarity");
    familiarity.encounters = required_count(value, "encounters", "familiarity");
    familiarity.new_authors = required_count(value, "new_authors", "familiarity");
    familiarity.accretion = required_ratio(value, "accretion", "familiarity");
    return familiarity;
}

void parse_trace(const cJSON *value, MetricTrace &trace) {
    trace.episodes = required_count(value, "episodes", "trace");
    trace.events = required_count(value, "events", "trace");
    trace.resolutions = required_count(value, "resolutions", "trace");
    trace.valence_updates = required_count(value, "valence_updates", "trace");
    const cJSON *new_authors = cJSON_GetObjectItemCaseSensitive(value, "new_authors");
    if (!cJSON_IsArray(new_authors)) {
        fail("'new_authors' field of trace is missing or not an array");
    }
    for (const cJSON *item = new_authors->child; item != nullptr; item = item->next) {
        if (!cJSON_IsString(item) || item->valuestring == nullptr) {
            fail("'new_authors' field of trace holds a non-string entry");
        }
        trace.new_authors.push_back(item->valuestring);
    }
    const cJSON *top_valence = cJSON_GetObjectItemCaseSensitive(value, "top_valence");
    if (!cJSON_IsArray(top_valence)) {
        fail("'top_valence' field of trace is missing or not an array");
    }
    for (const cJSON *item = top_valence->child; item != nullptr; item = item->next) {
        if (!cJSON_IsObject(item)) {
            fail("'top_valence' field of trace holds a non-object entry");
        }
        MetricGroup group;
        group.token = required_string(item, "token", "trace.top_valence[]");
        group.kind = required_string(item, "kind", "trace.top_valence[]");
        group.signal_sum = required_signed(item, "sum", "trace.top_valence[]");
        group.count = required_count(item, "count", "trace.top_valence[]");
        trace.top_valence.push_back(std::move(group));
    }
}

} // namespace

std::string serialise_metric_snapshot(const MetricSnapshot &snapshot) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        throw std::runtime_error("failed to allocate metric snapshot record");
    }
    cJSON_AddStringToObject(root, "format", "atperson-metrics");
    cJSON_AddNumberToObject(root, "version", kMetricFormatVersion);
    cJSON_AddStringToObject(root, "id", snapshot.id.c_str());
    cJSON_AddStringToObject(root, "at", snapshot.at.c_str());
    cJSON_AddStringToObject(root, "period_start", snapshot.period_start.c_str());
    cJSON_AddStringToObject(root, "period_end", snapshot.period_end.c_str());
    cJSON_AddNumberToObject(root, "cadence_seconds",
                            static_cast<double>(snapshot.cadence_seconds));
    if (snapshot.has_previous && !snapshot.previous_id.empty()) {
        cJSON *previous = cJSON_CreateObject();
        cJSON_AddStringToObject(previous, "id", snapshot.previous_id.c_str());
        cJSON_AddStringToObject(previous, "at", snapshot.previous_at.c_str());
        cJSON_AddItemToObject(root, "previous", previous);
    } else {
        cJSON_AddNullToObject(root, "previous");
    }

    cJSON *metrics = cJSON_CreateObject();
    cJSON *actions = cJSON_CreateObject();
    cJSON_AddNumberToObject(actions, "attempts", static_cast<double>(snapshot.actions.attempts));
    cJSON_AddNumberToObject(actions, "executed", static_cast<double>(snapshot.actions.executed));
    cJSON_AddNumberToObject(actions, "denied", static_cast<double>(snapshot.actions.denied));
    cJSON_AddNumberToObject(actions, "deferred", static_cast<double>(snapshot.actions.deferred));
    cJSON_AddNumberToObject(actions, "failed", static_cast<double>(snapshot.actions.failed));
    cJSON_AddNumberToObject(actions, "dry_run", static_cast<double>(snapshot.actions.dry_run));
    cJSON_AddNumberToObject(actions, "success_rate",
                            static_cast<double>(ratio_to_permille(snapshot.actions.success_rate)));
    cJSON_AddItemToObject(metrics, "actions", actions);

    cJSON *interaction = cJSON_CreateObject();
    cJSON_AddNumberToObject(interaction, "invites",
                            static_cast<double>(snapshot.interaction.invites));
    cJSON_AddNumberToObject(interaction, "invites_replied",
                            static_cast<double>(snapshot.interaction.invites_replied));
    cJSON_AddNumberToObject(interaction, "invites_expired",
                            static_cast<double>(snapshot.interaction.invites_expired));
    cJSON_AddNumberToObject(interaction, "invites_pending",
                            static_cast<double>(snapshot.interaction.invites_pending));
    cJSON_AddNumberToObject(interaction, "success_rate",
                            static_cast<double>(ratio_to_permille(snapshot.interaction.success_rate)));
    cJSON_AddItemToObject(metrics, "interaction", interaction);

    cJSON *reply_ratio = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply_ratio, "events",
                            static_cast<double>(snapshot.reply_ratio.events));
    cJSON_AddNumberToObject(reply_ratio, "replies",
                            static_cast<double>(snapshot.reply_ratio.replies));
    cJSON_AddNumberToObject(reply_ratio, "ratio",
                            static_cast<double>(ratio_to_permille(snapshot.reply_ratio.ratio)));
    cJSON_AddItemToObject(metrics, "reply_ratio", reply_ratio);

    cJSON *valence = cJSON_CreateObject();
    cJSON_AddNumberToObject(valence, "updates", static_cast<double>(snapshot.valence.updates));
    cJSON_AddNumberToObject(valence, "tokens", static_cast<double>(snapshot.valence.tokens));
    cJSON_AddNumberToObject(valence, "drift",
                            static_cast<double>(signed_to_permille(snapshot.valence.drift)));
    cJSON_AddItemToObject(metrics, "valence", valence);

    cJSON *familiarity = cJSON_CreateObject();
    cJSON_AddNumberToObject(familiarity, "authors",
                            static_cast<double>(snapshot.familiarity.authors));
    cJSON_AddNumberToObject(familiarity, "encounters",
                            static_cast<double>(snapshot.familiarity.encounters));
    cJSON_AddNumberToObject(familiarity, "new_authors",
                            static_cast<double>(snapshot.familiarity.new_authors));
    cJSON_AddNumberToObject(familiarity, "accretion",
                            static_cast<double>(ratio_to_permille(snapshot.familiarity.accretion)));
    cJSON_AddItemToObject(metrics, "familiarity", familiarity);
    cJSON_AddItemToObject(root, "metrics", metrics);

    cJSON *trace = cJSON_CreateObject();
    cJSON_AddNumberToObject(trace, "episodes", static_cast<double>(snapshot.trace.episodes));
    cJSON_AddNumberToObject(trace, "events", static_cast<double>(snapshot.trace.events));
    cJSON_AddNumberToObject(trace, "resolutions",
                            static_cast<double>(snapshot.trace.resolutions));
    cJSON_AddNumberToObject(trace, "valence_updates",
                            static_cast<double>(snapshot.trace.valence_updates));
    cJSON *new_authors = cJSON_CreateArray();
    for (const std::string &author : snapshot.trace.new_authors) {
        cJSON_AddItemToArray(new_authors, cJSON_CreateString(author.c_str()));
    }
    cJSON_AddItemToObject(trace, "new_authors", new_authors);
    cJSON *top_valence = cJSON_CreateArray();
    for (const MetricGroup &group : snapshot.trace.top_valence) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "token", group.token.c_str());
        cJSON_AddStringToObject(item, "kind", group.kind.c_str());
        cJSON_AddNumberToObject(item, "sum",
                                static_cast<double>(signed_to_permille(group.signal_sum)));
        cJSON_AddNumberToObject(item, "count", static_cast<double>(group.count));
        cJSON_AddItemToArray(top_valence, item);
    }
    cJSON_AddItemToObject(trace, "top_valence", top_valence);
    cJSON_AddItemToObject(root, "trace", trace);

    char *raw = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!raw) {
        throw std::runtime_error("failed to serialise metric snapshot record");
    }
    std::string json(raw);
    cJSON_free(raw);
    return json;
}

MetricSnapshot parse_metric_snapshot(std::string_view json) {
    cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        fail("record is not a JSON object");
    }
    MetricSnapshot snapshot;
    try {
        const std::string format = required_string(root, "format", "record");
        if (format != "atperson-metrics") {
            fail("unknown format '" + format + "'");
        }
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        if (!cJSON_IsNumber(version) ||
            static_cast<std::uint32_t>(version->valuedouble) != kMetricFormatVersion) {
            fail("unsupported version");
        }
        snapshot.id = required_string(root, "id", "record");
        snapshot.at = required_string(root, "at", "record");
        snapshot.period_start = required_string(root, "period_start", "record");
        snapshot.period_end = required_string(root, "period_end", "record");
        snapshot.cadence_seconds =
            required_count(root, "cadence_seconds", "record");

        const cJSON *previous = cJSON_GetObjectItemCaseSensitive(root, "previous");
        if (previous != nullptr && !cJSON_IsNull(previous)) {
            if (!cJSON_IsObject(previous)) {
                fail("'previous' field of record is not an object");
            }
            snapshot.has_previous = true;
            snapshot.previous_id = required_string(previous, "id", "record.previous");
            snapshot.previous_at = required_string(previous, "at", "record.previous");
        }

        const cJSON *metrics = required_object(root, "metrics", "record");
        snapshot.actions = parse_actions(required_object(metrics, "actions", "metrics"));
        snapshot.interaction =
            parse_interaction(required_object(metrics, "interaction", "metrics"));
        snapshot.reply_ratio =
            parse_reply_ratio(required_object(metrics, "reply_ratio", "metrics"));
        snapshot.valence = parse_valence(required_object(metrics, "valence", "metrics"));
        snapshot.familiarity =
            parse_familiarity(required_object(metrics, "familiarity", "metrics"));

        const cJSON *trace = required_object(root, "trace", "record");
        parse_trace(trace, snapshot.trace);
    } catch (...) {
        cJSON_Delete(root);
        throw;
    }
    cJSON_Delete(root);
    return snapshot;
}

void write_metric_snapshot(const std::filesystem::path &dir, const MetricSnapshot &snapshot) {
    if (snapshot.id.empty()) {
        throw std::runtime_error("metric snapshot requires an id");
    }
    if (snapshot.at.empty() || snapshot.period_start.empty() || snapshot.period_end.empty()) {
        throw std::runtime_error("metric snapshot requires complete period timestamps");
    }
    write_record(dir, snapshot.id, serialise_metric_snapshot(snapshot));
}

std::vector<MetricSnapshot> load_metric_snapshots(const std::filesystem::path &dir) {
    std::vector<MetricSnapshot> snapshots;
    const std::vector<std::filesystem::path> files = list_record_files(dir);
    for (const std::filesystem::path &path : files) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open metric snapshot " + path.string());
        }
        const std::string raw{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
        if (!input.bad() && raw.empty()) {
            continue;
        }
        MetricSnapshot snapshot = parse_metric_snapshot(raw);
        if (snapshot.id != path.stem().string()) {
            fail("record id '" + snapshot.id + "' does not match file " +
                 path.filename().string());
        }
        snapshots.push_back(std::move(snapshot));
    }
    /* Record keys encode creation time, so id order is read (append) order. */
    std::sort(snapshots.begin(), snapshots.end(),
              [](const MetricSnapshot &a, const MetricSnapshot &b) { return a.id < b.id; });
    return snapshots;
}

} // namespace atperson
