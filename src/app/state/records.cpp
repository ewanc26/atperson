#include "state/records.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

namespace atperson {
namespace {

/* fsync a directory so an atomically renamed record survives a crash after
 * the caller returns. */
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

std::filesystem::path record_path(const std::filesystem::path &dir, std::string_view id) {
    return dir / (std::string(id) + ".json");
}

bool record_exists(const std::filesystem::path &dir, std::string_view id) {
    std::error_code ec;
    return std::filesystem::exists(record_path(dir, id), ec);
}

void write_record(const std::filesystem::path &dir, std::string_view id,
                  std::string_view payload) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("cannot create record store " + dir.string() + ": " +
                                 ec.message());
    }
    const std::filesystem::path path = record_path(dir, id);
    if (std::filesystem::exists(path, ec)) {
        throw std::runtime_error("record " + path.string() + " already exists");
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create record " + tmp.string());
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            throw std::runtime_error("failed while writing record " + tmp.string());
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        throw std::runtime_error("could not commit record " + path.string() + ": " +
                                 rename_ec.message());
    }
    sync_parent_directory(dir);
}

std::vector<std::filesystem::path> list_record_files(const std::filesystem::path &dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec) || it->path().extension() != ".json") {
            continue;
        }
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end(),
              [](const std::filesystem::path &a, const std::filesystem::path &b) {
                  return a.filename() < b.filename();
              });
    return files;
}

std::string new_record_id() {
    static constexpr char alphabet[] = "234567abcdefghijklmnopqrstuvwxyz";
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::uint64_t micros =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now)
                                       .count());
    std::string id(13u, '2');
    for (std::size_t i = 0u; i < 13u; ++i) {
        id[12u - i] = alphabet[(micros >> (5u * static_cast<unsigned>(i))) & 0x1Fu];
    }
    return id;
}

} // namespace atperson