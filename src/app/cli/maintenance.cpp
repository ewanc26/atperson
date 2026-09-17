// Ledger maintenance command atoms: rebuild, compact, withdraw.
//
// Implementation of the contracts in ledger.hpp. Every body here
// is moved byte-faithfully from the original single-file dispatch in
// src/app/main.cpp; behaviour, ordering and output text are unchanged.

#include "ledger.hpp"

#include "atperson/core.h"
#include "atperson/graph.hpp"
#include "atperson/ledger.hpp"
#include "journal/store.hpp"
#include "lock.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atperson {
namespace cli {

int run_rebuild(
    std::ostream &out, const RuntimeResourceStatus &resource_status,
    const std::filesystem::path &data_dir, const std::filesystem::path &ledger_file,
    const std::filesystem::path &model_path,
    const std::vector<std::filesystem::path> &resource_paths,
    const ResourceOverrides &resource_overrides,
    const std::filesystem::path &journal_file,
    const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to rebuild from");
    }
    atperson::Ledger ledger(ledger_file);
    atperson::LanguageGraph rebuilt;
    auto rebuild_resources = atperson::refresh_runtime_resources(
        rebuilt, resource_paths, resource_overrides);
    atperson::require_runtime_write_headroom(rebuild_resources);
    const auto report = rebuilt.replay(ledger);

    /* Replay the journal's explicit valence events after the ledger so the
     * rebuilt state includes experience-derived valence. The journal is the
     * authority for self-authored experience; the ledger is the authority for
     * third-party observation. Replay order is deterministic: ledger entries
     * in id order, then journal valence entries in append order. */
    const JournalContents journal = atperson::load_journal(journal_file);
    for (const JournalValence &entry : journal.valence) {
        const std::optional<atp_valence_kind> kind = valence_kind_from_name(entry.kind);
        if (!kind.has_value()) {
            throw std::runtime_error("journal valence entry has unknown kind '" +
                                     entry.kind + "'");
        }
        rebuilt.valence_event(entry.token, kind.value(), entry.signal,
                              entry.at_epoch, entry.source);
    }

    rebuilt.save(model_path);
    out << "replayed " << report.replayed << " observation(s) from the ledger"
        << " (mirrored " << report.mirrored << " skipped, excluded "
        << report.excluded_pending << " pending, " << report.excluded_failed
        << " failed, " << report.excluded_withdrawn << " withdrawn)\n";
    out << "replayed " << journal.valence.size() << " valence event(s) from the journal\n";
    print_stats(rebuilt);
    return 0;
}

int run_compact(std::ostream &out, const RuntimeResourceStatus &resource_status,
                const std::filesystem::path &data_dir,
                const std::filesystem::path &ledger_file) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to compact");
    }
    atperson::Ledger ledger(ledger_file);
    const auto report = ledger.compact();
    out << "compacted " << report.entries << " entr"
        << (report.entries == 1u ? "y" : "ies") << " ("
        << report.patches_flattened << " patch record(s) flattened, "
        << report.payloads_dropped << " withdrawn payload(s) dropped)\n"
        << "ledger " << report.bytes_before << " -> " << report.bytes_after
        << " bytes\n";
    return 0;
}

int run_withdraw(std::ostream &out, const RuntimeResourceStatus &resource_status,
                 const std::filesystem::path &data_dir,
                 const std::filesystem::path &ledger_file, std::string_view scope,
                 std::string_view target,
                 const std::function<void(std::ostream &)> &usage) {
    atperson::require_runtime_write_headroom(resource_status);
    const atperson::StateLock writer_lock(data_dir);
    if (!std::filesystem::exists(ledger_file)) {
        throw std::runtime_error("no ledger at " + ledger_file.string() +
                                 "; nothing to withdraw from");
    }
    atperson::Ledger ledger(ledger_file);
    if (scope == "id") {
        const std::uint64_t id = std::strtoull(std::string(target).c_str(), nullptr, 10);
        ledger.withdraw(id);
        out << "withdrew observation " << id << "\n";
    } else if (scope == "source") {
        const std::size_t withdrawn = ledger.withdraw_source(target);
        out << "withdrew " << withdrawn << " observation(s) from " << target << '\n';
    } else if (scope == "author") {
        const std::size_t withdrawn = ledger.withdraw_author(target);
        out << "withdrew " << withdrawn << " observation(s) by " << target << '\n';
    } else {
        usage(std::cerr);
        return 2;
    }
    out << "run `atperson rebuild` to apply the withdrawal to learned state\n";
    return 0;
}

} // namespace cli
} // namespace atperson
