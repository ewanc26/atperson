#ifndef ATPERSON_TESTS_FAKE_PDS_HPP
#define ATPERSON_TESTS_FAKE_PDS_HPP

// An in-memory stand-in for the entity's own PDS, for tests that need both
// halves of the network-native state round trip (#142) without a network.
//
// One object is both the publish side and the reconstruct side, so a
// "publish then rebuild on a new host" test uses a single store and cannot
// accidentally pass because two fakes disagree:
//
//   as OutboundWriter  — replicate_drain() writes records into it
//   as RecordSource    — reconstruct_state() reads them back out
//
// It is deliberately a fixture, not a model of a PDS. It does not validate
// lexicons, sign anything, enforce rkey rules, paginate, or fail on
// anything the real service would reject. Its only fidelity guarantee is
// the one the round trip depends on: a record written is the record read
// back, in stable rkey order. Anything a test wants to fail must be asked
// for explicitly through `fail_next` or by withholding content.
//
// Ownership: the caller owns the FakePds. It holds no resources and needs
// no teardown.

#include "outbound/execute.hpp"
#include "replicate/reconstruct.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atperson::e2e {

class FakePds final : public OutboundWriter, public RecordSource {
  public:
    /* publish side */
    [[nodiscard]] std::string resolve_record_cid(const std::string &at_uri) override;
    OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                   const std::string &record_json) override;

    /* reconstruct side */
    [[nodiscard]] std::vector<std::string> list_records(std::string_view collection) override;
    [[nodiscard]] std::optional<std::string> get_record(std::string_view collection,
                                                        std::string_view rkey) override;
    [[nodiscard]] std::optional<std::string> fetch_content(std::string_view source_uri) override;

    /* Fixture controls. */
    /* Make the next put throw, as a network outage would. */
    void fail_next_put() noexcept {
        fail_next_ = true;
    }
    /* The content a source URI will serve back on fetch. Without it,
     * reconstruct must refuse the observation: an unverifiable record
     * never enters the ledger. */
    void serve_content(std::string source_uri, std::string text);
    /* Publish content for every observation whose source URI is in the
     * ledger, from the texts the feed used, so a round trip can be
     * verified end to end. */
    void serve_all_contents(const std::map<std::string, std::string> &uri_to_text);

    /* How many records are stored, across all collections. */
    [[nodiscard]] std::size_t size() const noexcept {
        return records_.size();
    }

  private:
    std::map<std::pair<std::string, std::string>, std::string> records_;
    std::map<std::string, std::string> content_;
    bool fail_next_{false};
};

} // namespace atperson::e2e

#endif // ATPERSON_TESTS_FAKE_PDS_HPP
