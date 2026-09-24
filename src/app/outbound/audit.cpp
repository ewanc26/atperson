#include "audit.hpp"

#include <cJSON.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace atperson {
namespace {

struct JsonDelete {
    void operator()(cJSON *value) const noexcept {
        cJSON_Delete(value);
    }
};

using Json = std::unique_ptr<cJSON, JsonDelete>;

} // namespace

const char *outbound_audit_outcome_name(OutboundAuditOutcome outcome) noexcept {
    switch (outcome) {
    case OutboundAuditOutcome::Executed:
        return "executed";
    case OutboundAuditOutcome::DryRun:
        return "dry_run";
    case OutboundAuditOutcome::Denied:
        return "denied";
    case OutboundAuditOutcome::Deferred:
        return "deferred";
    case OutboundAuditOutcome::Failed:
        return "failed";
    }
    return "denied";
}

std::string serialise_outbound_audit_entry(const OutboundAuditEntry &entry) {
    Json root(cJSON_CreateObject());
    if (!root) {
        throw std::runtime_error("failed to allocate outbound audit entry");
    }
    cJSON_AddStringToObject(root.get(), "at", entry.at.c_str());
    cJSON_AddStringToObject(root.get(), "kind", entry.kind.c_str());
    cJSON_AddStringToObject(root.get(), "rkey", entry.rkey.c_str());
    cJSON_AddStringToObject(root.get(), "digest", entry.digest.c_str());
    cJSON_AddStringToObject(root.get(), "outcome", outbound_audit_outcome_name(entry.outcome));
    cJSON_AddStringToObject(root.get(), "reason", entry.reason.c_str());
    cJSON_AddStringToObject(root.get(), "detail", entry.detail.c_str());
    cJSON_AddStringToObject(root.get(), "uri", entry.uri.c_str());
    cJSON_AddStringToObject(root.get(), "cid", entry.cid.c_str());
    if (!entry.envelope_id.empty()) {
        cJSON_AddStringToObject(root.get(), "envelope_id", entry.envelope_id.c_str());
    }
    if (!entry.attestation_cid.empty()) {
        cJSON_AddStringToObject(root.get(), "attestation_cid", entry.attestation_cid.c_str());
        cJSON_AddStringToObject(root.get(), "attestation_signature", entry.attestation_signature.c_str());
        cJSON_AddStringToObject(root.get(), "attestation_public_key", entry.attestation_public_key.c_str());
        cJSON_AddStringToObject(root.get(), "attestation_key_id", entry.attestation_key_id.c_str());
        cJSON_AddStringToObject(root.get(), "attestation_algorithm", entry.attestation_algorithm.c_str());
    }

    char *printed = cJSON_PrintUnformatted(root.get());
    if (!printed) {
        throw std::runtime_error("failed to serialise outbound audit entry");
    }
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

void append_outbound_audit(const std::filesystem::path &path, const OutboundAuditEntry &entry) {
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        throw std::runtime_error("cannot append outbound audit log " + path.string());
    }
    file << serialise_outbound_audit_entry(entry) << '\n';
    file.flush();
    if (!file) {
        throw std::runtime_error("failed while writing outbound audit log " + path.string());
    }
}

} // namespace atperson
