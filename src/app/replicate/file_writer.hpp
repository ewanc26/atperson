#ifndef ATPERSON_REPLICATE_FILE_WRITER_HPP
#define ATPERSON_REPLICATE_FILE_WRITER_HPP

// Offline record store (#142): an OutboundWriter that writes each
// record's JSON to `<records-dir>/<collection>/<rkey>.json` instead of
// the network. Used by `statepub drain --offline` so the exact bytes that
// would be published are durably staged in the central data directory —
// inspectable, diffable, and ready to push when the network returns.
//
// The file layout mirrors the AT Protocol repository exactly: one
// directory per collection, one file per rkey. A later online drain
// republishes from the same cursor (putRecord is idempotent on
// (collection, rkey)), so offline and online passes interleave freely.
//
// Writes are atomic (temp file + rename) and fsynced. resolve_record_cid
// is not meaningful offline and fails loudly — the publisher never calls
// it.

#include "../outbound/execute.hpp"

#include <filesystem>
#include <string>

namespace atperson {

class FileWriter final : public OutboundWriter {
  public:
    /* `records_dir` is created on first write. */
    explicit FileWriter(std::filesystem::path records_dir);

    std::string resolve_record_cid(const std::string &at_uri) override;
    OutboundWriteResult put_record(const std::string &collection, const std::string &rkey,
                                   const std::string &record_json) override;

  private:
    std::filesystem::path records_dir_;
};

} // namespace atperson

#endif
