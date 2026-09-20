// CLI local ingestion commands: ingest, ingest-file.
//
// Implementation of the contracts in ingest.hpp. Every body here is moved
// byte-faithfully from the original single-file dispatch in src/app/main.cpp;
// behaviour, ordering and output text are unchanged.

#include "ingest.hpp"

#include "config.hpp"
#include "lock.hpp"

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
}

int run_ingest(std::ostream &, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir, LanguageGraph &graph,
               const std::filesystem::path &model_path, std::string_view text,
               const char *source_value,
               const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    atperson::require_runtime_input_headroom(resource_status, std::strlen(text.data()));
    const auto before = graph.stats().training_steps;
    const atperson::StateLock writer_lock(data_dir);
    const std::string source = source_value ? source_value : "local:manual";
    graph.observe(text, source);
    graph.save(model_path);
    print_stats(graph);
    offer_external_publishing(std::cout, before, graph);
    return 0;
}

int run_ingest_file(std::ostream &, const RuntimeResourceStatus &resource_status,
                    const std::filesystem::path &data_dir, LanguageGraph &graph,
                    const std::filesystem::path &model_path,
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
    graph.observe(text, source);
    graph.save(model_path);
    print_stats(graph);
    offer_external_publishing(std::cout, before, graph);
    return 0;
}

} // namespace cli
} // namespace atperson
