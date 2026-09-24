#ifndef ATPERSON_STATE_RECORDS_HPP
#define ATPERSON_STATE_RECORDS_HPP

// The record-per-file store contract shared by every small JSON record
// surface (thoughts #142, self-eval metrics). One record is one file named
// `<id>.json` in a flat directory; writes are atomic (temp + flush + rename +
// parent fsync), refuse to overwrite an existing id, and enumeration is
// deterministic in ascending id order. Schema and serialisation are owned by
// the caller's store atom; this module owns only the file mechanics and the
// TID generator.
//
// Failure semantics: any I/O failure throws std::runtime_error without leaving
// a partial record behind. `list_record_files` never throws for a missing
// directory (yields an empty list).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

/* Atomically persist `payload` as `<dir>/<id>.json`, creating the directory
 * when missing. Refuses to overwrite an existing record. Throws
 * std::runtime_error on failure. */
void write_record(const std::filesystem::path &dir, std::string_view id,
                  std::string_view payload);

/* The path a record will occupy. */
[[nodiscard]] std::filesystem::path record_path(const std::filesystem::path &dir,
                                                std::string_view id);

/* True when `<dir>/<id>.json` already exists. */
[[nodiscard]] bool record_exists(const std::filesystem::path &dir, std::string_view id);

/* Every *.json regular file under `dir`, in ascending filename order. A
 * missing directory yields an empty vector. Temporary and stray files are
 * ignored. */
[[nodiscard]] std::vector<std::filesystem::path>
list_record_files(const std::filesystem::path &dir);

/* TID-shaped record id serving as creation order: lexicographic id order ==
 * append order. Encodes the wall-clock epoch in base32. */
[[nodiscard]] std::string new_record_id();

} // namespace atperson

#endif