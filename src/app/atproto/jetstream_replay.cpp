#include "jetstream_replay.hpp"

#include "jetstream.hpp"

#include "wolfram/jetstream_replay.h"

#include <cJSON.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

namespace atperson {
namespace {

std::string rfc3339_from_micros(std::int64_t micros) {
    const std::time_t seconds = static_cast<std::time_t>(micros / 1000000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec);
    return buffer;
}

} // namespace

void translate_jetstream_replay_events(
    const wf_jetstream_replay_event *events, std::size_t event_count,
    std::string_view self_did, const std::function<void(const JetstreamEvent &)> &on_event) {
    if (events == nullptr || !on_event) {
        throw std::invalid_argument("invalid Jetstream replay event arguments");
    }
    for (std::size_t i = 0u; i < event_count; ++i) {
        const auto &event = events[i];
        /* The replay API's commit rows are the only rows with record payloads.
         * Kind 1 is create and kind 2 is update; other kinds are identity,
         * handle, or account frames and do not enter the learning path. */
        if ((event.kind != 1u && event.kind != 2u) || event.collection == nullptr ||
            event.did == nullptr || event.rkey == nullptr || event.payload == nullptr ||
            event.payload_len == 0u) {
            continue;
        }

        cJSON *record = cJSON_ParseWithLength(
            reinterpret_cast<const char *>(event.payload), event.payload_len);
        if (record == nullptr || !cJSON_IsObject(record)) {
            cJSON_Delete(record);
            continue;
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *commit = cJSON_CreateObject();
        if (root == nullptr || commit == nullptr) {
            cJSON_Delete(root);
            cJSON_Delete(commit);
            cJSON_Delete(record);
            throw std::runtime_error("failed to allocate replay event envelope");
        }
        cJSON_AddStringToObject(root, "did", event.did);
        cJSON_AddStringToObject(root, "time", rfc3339_from_micros(event.witnessed_at).c_str());
        cJSON_AddStringToObject(commit, "operation", event.kind == 1u ? "create" : "update");
        cJSON_AddStringToObject(commit, "collection", event.collection);
        cJSON_AddStringToObject(commit, "rkey", event.rkey);
        cJSON_AddItemToObject(commit, "record", record);
        cJSON_AddItemToObject(root, "commit", commit);
        char *json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            SyncObservation observation;
            if (extract_jetstream_commit(json, std::strlen(json), self_did, observation)) {
                JetstreamEvent translated;
                translated.source_uri = std::move(observation.source_uri);
                translated.author_did = std::move(observation.author_did);
                translated.created_at = std::move(observation.created_at);
                translated.text = std::move(observation.text);
                translated.reply_root = std::move(observation.context.reply_root_uri);
                translated.reply_parent = std::move(observation.context.reply_parent_uri);
                translated.quote_uri = std::move(observation.context.quote_uri);
                translated.policy_reason = observation.policy_reason;
                translated.seq = static_cast<std::int64_t>(event.seq);
                on_event(translated);
            }
            cJSON_free(json);
        }
        cJSON_Delete(root);
    }
}

void decode_jetstream_replay_segment(
    const void *bytes, std::size_t bytes_len, std::string_view self_did,
    const std::function<void(const JetstreamEvent &)> &on_event) {
    if (bytes == nullptr || bytes_len == 0u || !on_event) {
        throw std::invalid_argument("invalid Jetstream replay segment arguments");
    }
    wf_jetstream_replay_event *events = nullptr;
    std::size_t event_count = 0u;
    const wf_status status =
        wf_jetstream_replay_segment_decode(bytes, bytes_len, &events, &event_count);
    if (status != WF_OK) {
        throw std::runtime_error("Wolfram failed to decode Jetstream replay segment");
    }
    try {
        translate_jetstream_replay_events(events, event_count, self_did, on_event);
    } catch (...) {
        wf_jetstream_replay_events_free(events, event_count);
        throw;
    }
    wf_jetstream_replay_events_free(events, event_count);
}

} // namespace atperson
