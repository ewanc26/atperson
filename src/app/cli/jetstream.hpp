/* CLI Jetstream public backfill command (#60).
 *
 * `atperson jetstream [max-events] [max-ms]` runs one bounded backfill cycle
 * over the unauthenticated public Jetstream JSON stream — no login, no app
 * password, no credentials of any kind. Jetstream derives from the AT Protocol
 * firehose but is not the binary `com.atproto.sync.subscribeRepos` wire format.
 * It ingests filtered commit envelopes through the same durable
 * pipeline as `sync` (ledger reservation -> remember -> outcome commit ->
 * action-event linkage) and checkpoints the Jetstream cursor only after every
 * event in the cycle has been durably handled.
 *
 * The cursor is an opaque Jetstream sequence number stored as a decimal
 * string in the dedicated Jetstream ingestion state; it is never parsed here.
 * After catch-up
 * exhaustion the cursor is cleared and the next independent backfill begins
 * at the feed head, relying on the durable ledger for deduplication.
 *
 * Failure modes: throws std::runtime_error on a fatal client error (connect
 * failure, parse failure of a frame the feed emitted). A WOULD_BLOCK return
 * from `JetstreamClient::fetch_batch` (reconnect backoff) is not a failure:
 * the caller sleeps for the advertised delay and retries the same batch.
 * Returns 0 on success.
 */

#ifndef ATPERSON_CLI_JETSTREAM_HPP
#define ATPERSON_CLI_JETSTREAM_HPP

#include "atperson/graph.hpp"
#include "runtime.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>

namespace atperson {
namespace cli {

int run_jetstream_status(
    std::ostream &out, const std::filesystem::path &state_file,
    const std::filesystem::path &collections_file,
    const std::filesystem::path &dids_file);

int run_jetstream(std::ostream &out, const RuntimeResourceStatus &resource_status,
                  const std::filesystem::path &data_dir, LanguageGraph &graph,
                  const std::filesystem::path &model_path,
                  const std::filesystem::path &ledger_file,
                  const std::filesystem::path &state_file,
                  const std::filesystem::path &collections_file,
                  const std::filesystem::path &dids_file,
                  const std::filesystem::path &kinds_file,
                  int max_events, int max_ms,
                  const std::function<void(const LanguageGraph &)> &print_stats);

int run_jetstream_archive(std::ostream &out,
                          const RuntimeResourceStatus &resource_status,
                          const std::filesystem::path &data_dir,
                          LanguageGraph &graph,
                          const std::filesystem::path &model_path,
                          const std::filesystem::path &ledger_file,
                          const std::filesystem::path &state_file,
                          std::optional<std::uint64_t> after_seq,
                          std::optional<std::uint64_t> before_seq,
                          std::optional<std::uint64_t> relative_span,
                          const std::filesystem::path &collections_file,
                          const std::filesystem::path &dids_file,
                          const std::function<void(const LanguageGraph &)> &print_stats);

} // namespace cli
} // namespace atperson

#endif
