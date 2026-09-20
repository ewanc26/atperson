#include "cli/protocol_command.hpp"

#include "cli/config.hpp"
#include "protocol.hpp"
#include "wolfram/identity.h"
#include "wolfram/xrpc.h"

#include <cstdlib>
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
    char **rotation_keys = nullptr;
    size_t rotation_key_count = 0;
    const wf_status rotation_status = wf_did_resolve_rotation_keys(
        client, did, &rotation_keys, &rotation_key_count);
    std::vector<std::string> rotation_key_values;
    if (rotation_status == WF_OK) {
        rotation_key_values.reserve(rotation_key_count);
        for (size_t i = 0; i < rotation_key_count; ++i) {
            rotation_key_values.emplace_back(rotation_keys[i]);
        }
    }
    wf_did_rotation_keys_free(rotation_keys, rotation_key_count);
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
