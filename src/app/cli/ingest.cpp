// CLI local ingestion commands: ingest, ingest-file.
//
// Implementation of the contracts in ingest.hpp. Every body here is moved
// byte-faithfully from the original single-file dispatch in src/app/main.cpp;
// behaviour, ordering and output text are unchanged.

#include "ingest.hpp"

#include "lock.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace atperson {
namespace cli {

int run_ingest(std::ostream &out, const RuntimeResourceStatus &resource_status,
               const std::filesystem::path &data_dir, LanguageGraph &graph,
               const std::filesystem::path &model_path, std::string_view text,
               const char *source_value,
               const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
    atperson::require_runtime_input_headroom(resource_status, std::strlen(text.data()));
    const atperson::StateLock writer_lock(data_dir);
    const std::string source = source_value ? source_value : "local:manual";
    graph.observe(text, source);
    graph.save(model_path);
    print_stats(graph);
    return 0;
}

int run_ingest_file(std::ostream &out, const RuntimeResourceStatus &resource_status,
                    const std::filesystem::path &data_dir, LanguageGraph &graph,
                    const std::filesystem::path &model_path,
                    const std::filesystem::path &input_path, const char *source_value,
                    const std::function<void(const LanguageGraph &)> &print_stats) {
    atperson::require_runtime_write_headroom(resource_status);
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
    return 0;
}

} // namespace cli
} // namespace atperson
