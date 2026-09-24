#include "file_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

namespace atperson {
namespace {

/* fsync the file's directory so a rename survives a crash. */
void sync_parent_directory(const std::filesystem::path &path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(parent, ec);
    if (ec) {
        canonical = parent;
    }
#if defined(_WIN32)
    (void)canonical;
#else
    const int fd = ::open(canonical.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

} // namespace

FileWriter::FileWriter(std::filesystem::path records_dir) : records_dir_(std::move(records_dir)) {}

std::string FileWriter::resolve_record_cid(const std::string &at_uri) {
    (void)at_uri;
    throw std::runtime_error("file writer: resolve_record_cid is a network operation");
}

OutboundWriteResult FileWriter::put_record(const std::string &collection, const std::string &rkey,
                                           const std::string &record_json) {
    /* The rkey is the only path component derived from record data; it is
     * a TID-shaped/base-32 identifier produced by this codebase, never
     * operator input. Reject separators defensively anyway. */
    if (rkey.find('/') != std::string::npos || rkey.find("..") != std::string::npos ||
        rkey.empty()) {
        throw std::runtime_error("file writer: invalid record key '" + rkey + "'");
    }
    const std::filesystem::path collection_dir = records_dir_ / collection;
    std::error_code ec;
    std::filesystem::create_directories(collection_dir, ec);

    const std::filesystem::path target = collection_dir / (rkey + ".json");
    const std::filesystem::path temp = collection_dir / (rkey + ".json.tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("file writer: cannot stage record at " + temp.string());
        }
        file << record_json;
        file.flush();
        if (!file) {
            throw std::runtime_error("file writer: failed writing record at " + temp.string());
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("file writer: cannot publish record at " + target.string());
    }
    sync_parent_directory(target);

    OutboundWriteResult result;
    /* A local file has no at-URI or CID; the uri field carries the file
     * path so the operator can find the staged record. */
    result.uri = target.string();
    result.cid = "";
    return result;
}

} // namespace atperson
