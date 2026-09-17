#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace atperson {

/* Refusal to acquire the writer lock: another live process owns the state
 * directory, or the lock is held by a process that cannot be proven stale. */
class StateLockError : public std::runtime_error {
  public:
    explicit StateLockError(const std::string &message) : std::runtime_error(message) {}
};

/* RAII ownership of the state directory's writer lock.
 *
 * The lock is a lockfile created with O_CREAT|O_EXCL inside the state
 * directory. The file records the owner's pid, a boot marker (see
 * state_lock.cpp), and the acquisition time. Release removes the file.
 *
 * Stale detection: a lockfile whose owner pid is no longer alive, or whose
 * boot marker differs from the current boot (the machine rebooted since the
 * lock was taken), is provably stale and is stolen. A lockfile owned by a
 * live process on the current boot is respected and acquisition fails with
 * a clear diagnostic.
 *
 * Consistency model: read-only commands run without the lock and observe
 * the snapshot/ledger as of their own read; a concurrent writer may commit
 * after the reader started. Only mutating commands take the lock. */
class StateLock {
  public:
    /* Acquires the state directory's default writer lock, or throws. */
    explicit StateLock(const std::filesystem::path &state_directory);

    /* Acquires a named lockfile inside `state_directory`. Used by the outbound
     * execution path (#25), which serialises its own budget read-modify-write
     * without competing with the daemon's long-held writer lock. The name must
     * be a bare filename: empty names and path separators are rejected. */
    StateLock(const std::filesystem::path &state_directory, std::string lock_file_name);

    ~StateLock();

    StateLock(const StateLock &) = delete;
    StateLock &operator=(const StateLock &) = delete;
    StateLock(StateLock &&) = delete;
    StateLock &operator=(StateLock &&) = delete;

    const std::filesystem::path &lock_path() const { return lock_path_; }

  private:
    std::filesystem::path lock_path_;
};

/* Inspects a lockfile without acquiring it. Returns a human-readable
 * description for diagnostics, or an empty string when no lock exists. */
std::string describe_state_lock(const std::filesystem::path &state_directory);

} // namespace atperson
