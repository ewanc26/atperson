#ifndef ATPERSON_LEDGER_HPP
#define ATPERSON_LEDGER_HPP

#include "atperson/core.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atperson {

enum class LedgerResult { New, ExistsPending, ExistsCommitted };

/**
 * Thin RAII wrapper over the C23 observation ledger.
 *
 * The ledger is the durable, append-only record of every observation fed to
 * the learning core and the authority for cross-run deduplication: a
 * (source_id, content_digest) pair with a committed outcome is never trained
 * on again, even after a process restart.
 */
class Ledger {
  public:
    explicit Ledger(const std::filesystem::path &path);
    ~Ledger();

    Ledger(const Ledger &) = delete;
    Ledger &operator=(const Ledger &) = delete;

    Ledger(Ledger &&other) noexcept;
    Ledger &operator=(Ledger &&other) noexcept;

    /** Stable content digest; the same value a restarted process recomputes. */
    static std::uint64_t digest(std::string_view content);

    /**
     * Record an observation under the unique (source id + digest) key.
     * Appending is durable before this returns. Real errors throw.
     */
    [[nodiscard]] LedgerResult append(std::string_view source_id, std::string_view author_did,
                                      std::uint64_t observed_at, std::uint64_t content_digest,
                                      std::uint32_t schema_version, atp_ledger_outcome outcome,
                                      std::uint64_t *out_id);

    /** Append a durable outcome patch for an existing entry. Throws on error. */
    void set_outcome(std::uint64_t id, atp_ledger_outcome outcome);

    /** Dedup query on the unique (source id + digest) index. */
    [[nodiscard]] LedgerResult lookup(std::string_view source_id, std::uint64_t content_digest,
                                      atp_ledger_entry *out_entry) const;

    [[nodiscard]] std::uint64_t count() const noexcept;
    [[nodiscard]] std::vector<atp_ledger_entry> entries() const;

  private:
    atp_ledger *ledger_{};
};

} // namespace atperson

#endif