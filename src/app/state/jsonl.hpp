#ifndef ATPERSON_STATE_JSONL_HPP
#define ATPERSON_STATE_JSONL_HPP

// Shared torn-tail repair for newline-delimited append-only files.
//
// A crash mid-append can leave a partial final line with no newline. Appending
// after it glues the new line onto those bytes and yields a complete but
// malformed line, which corrupts the file for good. Every append-only JSONL
// store calls repair_torn_tail(path) immediately before appending. Header-only
// so each target that already builds such a store needs no extra source file.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace atperson {

/* Drop the bytes after the last newline, if the file does not end in one.
 * A missing or empty file, or one that already ends in a newline, is left
 * alone (a single byte is read). Throws std::runtime_error on I/O failure. */
inline void repair_torn_tail(const std::filesystem::path &path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0u) {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot inspect " + path.string());
    }
    input.seekg(static_cast<std::streamoff>(size - 1u));
    char last = '\0';
    input.get(last);
    if (last == '\n') {
        return;
    }
    std::uintmax_t keep = 0u;
    std::uintmax_t position = size;
    std::vector<char> chunk(4096u);
    while (position > 0u) {
        const std::uintmax_t length = std::min<std::uintmax_t>(chunk.size(), position);
        position -= length;
        input.clear();
        input.seekg(static_cast<std::streamoff>(position));
        input.read(chunk.data(), static_cast<std::streamsize>(length));
        if (!input) {
            throw std::runtime_error("cannot read " + path.string());
        }
        bool found = false;
        for (std::uintmax_t i = length; i > 0u; --i) {
            if (chunk[static_cast<std::size_t>(i - 1u)] == '\n') {
                keep = position + i;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    input.close();
    std::filesystem::resize_file(path, keep, ec);
    if (ec) {
        throw std::runtime_error("cannot repair torn tail of " + path.string() + ": " +
                                 ec.message());
    }
}

} // namespace atperson

#endif
