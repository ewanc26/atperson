#include "cli/protocol_command.hpp"

#include "cli/config.hpp"
#include "protocol.hpp"
#include "wolfram/identity.h"
#include "wolfram/xrpc.h"

#include <cstdlib>
#include <ctime>
#include <ostream>

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
    const auto identity = protocol::accept_identity(
        did, handle, "wolfram:identity", document.signing_key,
        document.pds_endpoint, protocol::Verification::Verified);
    if (!identity) {
        err << "protocol resolve: Wolfram returned invalid identity data\n";
        free(did);
        wf_did_document_free(&document);
        wf_xrpc_client_free(client);
        return 1;
    }
    protocol::ProtocolEvidence evidence{
        protocol::EvidenceKind::Identity,
        service,
        "identity",
        did,
        std::string(handle) + "|" + document.pds_endpoint + "|" + document.signing_key,
        0,
        static_cast<std::uint64_t>(std::time(nullptr)),
        protocol::Verification::Verified,
        1.0};
    protocol::EvidenceLedger ledger(protocol_ledger_path());
    const bool added = ledger.append(std::move(evidence));
    out << (added ? "learned" : "duplicate") << " identity " << handle << " -> " << did
        << " pds=" << document.pds_endpoint << '\n';
    free(did);
    wf_did_document_free(&document);
    wf_xrpc_client_free(client);
    return 0;
}

} // namespace atperson::cli
