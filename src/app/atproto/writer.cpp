#include "writer.hpp"

#include <wolfram/repo_typed.h>
#include <wolfram/syntax.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace atperson {

std::string WolframWriter::resolve_record_cid(const std::string &at_uri) {
    wf_syntax_aturi parsed{};
    const int ok = wf_syntax_aturi_parse(at_uri.c_str(), &parsed);
    if (ok == 0 || !parsed.authority || !parsed.collection || !parsed.record_key) {
        wf_syntax_aturi_free(&parsed);
        throw std::runtime_error("invalid at-URI for reply context: " + at_uri);
    }

    wf_repo_record record{};
    const wf_status status = wf_agent_get_record_typed(
        session_.agent(), parsed.authority, parsed.collection, parsed.record_key, nullptr, &record);
    wf_syntax_aturi_free(&parsed);
    if (status != WF_OK) {
        wf_repo_record_free(&record);
        throw wolfram_error("reply record fetch", status);
    }
    if (!record.has_cid || !record.cid || record.cid[0] == '\0') {
        wf_repo_record_free(&record);
        throw std::runtime_error("reply record has no content CID: " + at_uri);
    }

    const std::string cid = record.cid ? record.cid : "";
    wf_repo_record_free(&record);
    return cid;
}

OutboundWriteResult WolframWriter::put_record(const std::string &collection,
                                              const std::string &rkey,
                                              const std::string &record_json) {
    if (rkey.empty() || wf_syntax_record_key_is_valid(rkey.c_str()) == 0) {
        throw std::runtime_error("invalid outbound record key: " + rkey);
    }

    wf_repo_write_record_result result{};
    const wf_status status = wf_agent_put_record_typed(
        session_.agent(), session_.did().c_str(), collection.c_str(), rkey.c_str(),
        /*validate=*/1, record_json.c_str(), nullptr, nullptr, &result);
    if (status != WF_OK) {
        wf_repo_write_record_result_free(&result);
        throw wolfram_error("outbound putRecord", status);
    }

    OutboundWriteResult written;
    written.uri = result.uri ? result.uri : "";
    written.cid = result.cid ? result.cid : "";
    wf_repo_write_record_result_free(&result);
    if (written.uri.empty()) {
        throw std::runtime_error("outbound putRecord returned no record URI");
    }
    return written;
}

} // namespace atperson
