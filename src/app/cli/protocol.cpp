#include "cli/protocol_command.hpp"

#include "cli/config.hpp"
#include "protocol.hpp"
#include "wolfram/identity.h"
#include "wolfram/plc.h"
#include "wolfram/xrpc.h"

#include <cJSON.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <ostream>
#include <vector>

namespace atperson::cli {

int run_protocol_resolve(std::ostream &out, std::ostream &err,
                         const char *handle, const char *service) {
    wf_xrpc_client *client = wf_xrpc_client_new(service);
    if (!client) {
        err << "protocol resolve: failed to create transport\n";
        return 1;
    }
    char *did = nullptr;
    wf_status status = wf_handle_resolve(client, handle, &did);
    if (status != WF_OK || !did) {
        err << "protocol resolve: handle resolution failed (status "
            << static_cast<int>(status) << ")\n";
        wf_xrpc_client_free(client);
        return 1;
    }
    wf_did_document document{};
    status = wf_did_resolve(client, did, &document);
    if (status != WF_OK || !document.pds_endpoint || !document.signing_key) {
        err << "protocol resolve: DID document resolution failed (status "
            << static_cast<int>(status) << ")\n";
        free(did);
        wf_xrpc_client_free(client);
        wf_did_document_free(&document);
        return 1;
    }
    /* PLC rotation keys are not part of the trimmed DID document; recover
     * them from the directory's current published operation (Wolfram owns
     * the fetch, protocol semantics stay in ./protocol). did:web DIDs have
     * no PLC directory and therefore no PLC rotation keys. */
    std::vector<std::string> rotation_key_values;
    if (did != nullptr && strncmp(did, "did:plc:", 8) == 0) {
        char *last_cid = nullptr;
        char *last_op_json = nullptr;
        const wf_status op_status = wf_plc_get_last_op(
            client, "https://plc.directory", did, &last_cid, &last_op_json);
        if (op_status == WF_OK && last_cid != nullptr && last_op_json != nullptr) {
            cJSON *op = cJSON_Parse(last_op_json);
            const cJSON *rotation_keys = op != nullptr
                                             ? cJSON_GetObjectItemCaseSensitive(op, "rotationKeys")
                                             : nullptr;
            if (cJSON_IsArray(rotation_keys)) {
                rotation_key_values.reserve(static_cast<std::size_t>(cJSON_GetArraySize(rotation_keys)));
                cJSON *item = nullptr;
                cJSON_ArrayForEach(item, rotation_keys) {
                    if (cJSON_IsString(item) && item->valuestring != nullptr) {
                        rotation_key_values.emplace_back(item->valuestring);
                    }
                }
            }
            cJSON_Delete(op);
        }
        free(last_cid);
        free(last_op_json);
    }
    const auto identity = protocol::accept_identity(
        did, handle, "wolfram:identity", document.signing_key,
        document.pds_endpoint, protocol::Verification::Verified,
        std::move(rotation_key_values));
    if (!identity) {
        err << "protocol resolve: Wolfram returned invalid identity data\n";
        free(did);
        wf_did_document_free(&document);
        wf_xrpc_client_free(client);
        return 1;
    }
    protocol::EvidenceLedger ledger(protocol_ledger_path());
    const auto identity_fact = *identity;
    const bool added = protocol::append_identity_fact(
        ledger, identity_fact, service, 0, static_cast<std::uint64_t>(std::time(nullptr)));
    const auto observed_at = static_cast<std::uint64_t>(std::time(nullptr));
    if (const auto pds = protocol::accept_service_fact(
            protocol::ServiceRole::Pds, document.pds_endpoint, did,
            protocol::Verification::Verified)) {
        static_cast<void>(protocol::append_service_fact(
            ledger, *pds, service, 0, observed_at));
    }
    if (document.feedgen_endpoint != nullptr) {
        if (const auto feedgen = protocol::accept_service_fact(
                protocol::ServiceRole::FeedGenerator, document.feedgen_endpoint, did,
                protocol::Verification::Verified)) {
            static_cast<void>(protocol::append_service_fact(
                ledger, *feedgen, service, 0, observed_at));
        }
    }
    char *raw_document = nullptr;
    const wf_status raw_status = wf_did_resolve_raw(client, did, &raw_document);
    bool raw_added = false;
    if (raw_status == WF_OK && raw_document != nullptr) {
        protocol::ProtocolEvidence document_evidence{
            protocol::EvidenceKind::Identity,
            service,
            "did-document",
            did,
            raw_document,
            0,
            static_cast<std::uint64_t>(std::time(nullptr)),
            protocol::Verification::Verified,
            1.0};
        raw_added = ledger.append(std::move(document_evidence));
        free(raw_document);
    }
    out << (added ? "learned" : "duplicate") << " identity " << handle << " -> " << did
        << " pds=" << document.pds_endpoint
        << " did-document=" << (raw_added ? "learned" : "duplicate/unavailable") << '\n';
    free(did);
    wf_did_document_free(&document);
    wf_xrpc_client_free(client);
    return 0;
}

} // namespace atperson::cli
