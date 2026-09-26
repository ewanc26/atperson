/* The in-memory PDS fixture. See fake_pds.hpp for what it does and
 * deliberately does not model. */

#include "support/fake_pds.hpp"

#include <stdexcept>
#include <utility>

namespace atperson::e2e {

std::string FakePds::resolve_record_cid(const std::string &at_uri) {
    (void)at_uri;
    return "fake-cid";
}

OutboundWriteResult FakePds::put_record(const std::string &collection, const std::string &rkey,
                                        const std::string &record_json) {
    if (fail_next_) {
        fail_next_ = false;
        throw std::runtime_error("network down");
    }
    records_[{collection, rkey}] = record_json;
    OutboundWriteResult result;
    result.uri = "at://did:fake/" + collection + "/" + rkey;
    result.cid = "fake-cid";
    return result;
}

std::vector<std::string> FakePds::list_records(std::string_view collection) {
    /* std::map iterates in key order, so this is already the stable rkey
     * order reconstruct requires, filtered to one collection. */
    std::vector<std::string> rkeys;
    for (const auto &[key, value] : records_) {
        (void)value;
        if (key.first == collection) {
            rkeys.push_back(key.second);
        }
    }
    return rkeys;
}

std::optional<std::string> FakePds::get_record(std::string_view collection, std::string_view rkey) {
    const auto it = records_.find({std::string(collection), std::string(rkey)});
    if (it == records_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> FakePds::fetch_content(std::string_view source_uri) {
    const auto it = content_.find(std::string(source_uri));
    if (it == content_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FakePds::serve_content(std::string source_uri, std::string text) {
    content_[std::move(source_uri)] = std::move(text);
}

void FakePds::serve_all_contents(const std::map<std::string, std::string> &uri_to_text) {
    for (const auto &[uri, text] : uri_to_text) {
        serve_content(uri, text);
    }
}

} // namespace atperson::e2e
