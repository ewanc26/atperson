// CLI local ingestion commands: ingest, ingest-file.
//
// Implementation of the contracts in ingest.hpp. Local observations go through
// the same durable pipeline as sync (ledger reservation, dedup, learning,
// episodic memory), so they are deduplicated, survive `rebuild` and can be
// withdrawn like any other observation.

#include "ingest.hpp"

#include "config.hpp"
#include "lock.hpp"
#include "control/state.hpp"
#include "sync/engine.hpp"

#include <cstring>
#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace atperson {
namespace cli {

namespace {
void offer_external_publishing(std::ostream &out, std::uint64_t before,
                               const LanguageGraph &graph) {
    if (before != 0 || graph.stats().training_steps == 0 ||
        external_publishing_enabled() || !isatty(STDIN_FILENO)) return;
    out << "Initial neural training run completed. Enable external publishing by setting "
           "ATPERSON_ALLOW_EXTERNAL_PUBLISHING=true in your .env and sourcing it before "
           "the next run? [y/N] ";
    out.flush();
    char answer = '\0';
    if (std::cin.get(answer) && (answer == 'y' || answer == 'Y')) {
        out << "External publishing remains off for this process; set the env toggle and restart "
               "to activate it.\n";
    } else {
        out << "External publishing remains disabled.\n";
    }
}

// Feeds one local observation through the ledger-backed pipeline and saves the
// snapshot when it was new. Caller holds the writer lock.
void observe_local(std::ostream &out, LanguageGraph &graph,
                   const std::filesystem::path &model_path,
                   const std::filesystem::path &ledger_path, const std::string &text,
                   const std::string &source) {
    if (source.empty() || source.size() >= ATPERSON_LEDGER_SOURCE_BYTES) {
        throw std::runtime_error("source id must be 1.." +
                                 std::to_string(ATPERSON_LEDGER_SOURCE_BYTES - 1u) + " bytes");
    }
    Ledger ledger(ledger_path);
    SyncObservation observation;
    observation.text = text;
    observation.source_uri = source;
    observation.created_at = control_now_rfc3339();
    if (!process_observation(graph, ledger, observation)) {
        out << "already learned (source " << source << ", same content); nothing to do\n";
        return;
    }
    graph.save(model_path);
}
} // namespace

int run_ingest(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir, LanguageGraph &graph,
               const std::filesystem::path &model_path,
               const std::filesystem::path &ledger_path, std::string_view text,
               const char *source_value,
               const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    atperson::require_runtime_input_headroom(resource_status, text.size());
    const auto before = graph.stats().training_steps;
    const atperson::StateLock writer_lock(data_dir);
    const std::string source = source_value ? source_value : "local:manual";
    const std::string text_copy(text);
    observe_local(out, graph, model_path, ledger_path, text_copy, source);
    print_stats(graph);
    offer_external_publishing(std::cout, before, graph);
    return 0;
}

int run_ingest_file(std::ostream &out, const RuntimeResourceStatus &resource_status,
                    const std::filesystem::path &data_dir, LanguageGraph &graph,
                    const std::filesystem::path &model_path,
                    const std::filesystem::path &ledger_path,
                    const std::filesystem::path &input_path, const char *source_value,
                    const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    const auto before = graph.stats().training_steps;
    std::error_code size_error;
    const auto input_size = std::filesystem::file_size(input_path, size_error);
    if (size_error) {
        throw std::runtime_error("could not determine size of " + input_path.string());
    }
    atperson::require_runtime_input_headroom(resource_status, input_size);
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open " + input_path.string());
    }
    const atperson::StateLock writer_lock(data_dir);
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const std::string source =
        source_value ? source_value : "file:" + input_path.string();
    observe_local(out, graph, model_path, ledger_path, text, source);
    print_stats(graph);
    offer_external_publishing(std::cout, before, graph);
    return 0;
}

} // namespace cli
} // namespace atperson
