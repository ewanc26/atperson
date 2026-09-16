/* Offline tests for the state-directory writer lock. No network access.
 *
 * The competing-process acceptance test uses real forked processes: a child
 * holding the lock must block the parent, and after the child exits the
 * parent must acquire it. */

#include "state_lock.hpp"

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

const std::filesystem::path make_temp_dir(const char *tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                     (std::string("atperson-lock-") + tag + "-" +
                                      std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_lock_file(const std::filesystem::path &dir, const std::string &payload) {
    std::ofstream file(dir / ".writer-lock");
    file << payload;
}

bool acquire_fails(const std::filesystem::path &dir) {
    try {
        const atperson::StateLock lock(dir);
        return false;
    } catch (const atperson::StateLockError &) {
        return true;
    }
}

void test_acquire_release_round_trip() {
    const auto dir = make_temp_dir("roundtrip");
    {
        const atperson::StateLock lock(dir);
        assert(std::filesystem::exists(lock.lock_path()));
    }
    assert(!std::filesystem::exists(dir / ".writer-lock"));
    /* Re-acquiring after release works. */
    const atperson::StateLock second(dir);
    std::filesystem::remove_all(dir);
}

void test_second_acquire_fails() {
    const auto dir = make_temp_dir("second");
    const atperson::StateLock first(dir);
    assert(acquire_fails(dir));
    std::filesystem::remove_all(dir);
}

void test_lock_file_records_owner() {
    const auto dir = make_temp_dir("records");
    {
        const atperson::StateLock lock(dir);
        std::ifstream file(lock.lock_path());
        long long pid = 0;
        std::string boot;
        std::string created;
        assert((file >> pid >> boot >> created));
        assert(pid == static_cast<long long>(::getpid()));
        assert(!boot.empty());
        assert(!created.empty());
    }
    std::filesystem::remove_all(dir);
}

void test_stale_dead_pid_is_stolen() {
    const auto dir = make_temp_dir("deadpid");
    /* A lock owned by a pid that has exited is provably stale. */
    const pid_t child = ::fork();
    if (child == 0) {
        _exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    write_lock_file(dir, std::to_string(static_cast<long long>(child)) +
                            " boot:current 2026-09-16T00:00:00Z\n");
    /* The dead owner's lock is stale and gets stolen. */
    const atperson::StateLock lock(dir);
    std::filesystem::remove_all(dir);
}

void test_malformed_lock_is_stale() {
    const auto dir = make_temp_dir("malformed");
    write_lock_file(dir, "garbage that is not a lock\n");
    const atperson::StateLock lock(dir);
    std::filesystem::remove_all(dir);
}

/* An empty lockfile is the create/write window of a live owner. It must not
 * be stolen instantly; after the grace period it is treated as abandoned
 * (the owner died mid-write) and stolen. This test verifies the steal
 * happens within a bounded time rather than hanging or refusing forever. */
void test_empty_lock_is_graced_then_stolen() {
    const auto dir = make_temp_dir("emptylock");
    write_lock_file(dir, "");
    const auto started = std::chrono::steady_clock::now();
    const atperson::StateLock lock(dir);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    /* The grace period is ~50ms; allow generous slack for slow CI runners. */
    assert(elapsed >= 10);
    assert(elapsed < 5000);
    std::filesystem::remove_all(dir);
}

void test_describe_lock() {
    const auto dir = make_temp_dir("describe");
    assert(atperson::describe_state_lock(dir).empty());
    {
        const atperson::StateLock lock(dir);
        const std::string description = atperson::describe_state_lock(dir);
        assert(!description.empty());
        assert(description.find(std::to_string(static_cast<long long>(::getpid()))) !=
               std::string::npos);
    }
    assert(atperson::describe_state_lock(dir).empty());
    std::filesystem::remove_all(dir);
}

/* Competing processes: a child holds the lock and stays alive; the parent
 * must be refused. After the child exits (releasing the lock), the parent
 * must acquire it. */
void test_competing_processes() {
    const auto dir = make_temp_dir("competing");

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        /* Child: take the lock, signal readiness via a marker file, then
         * sleep until the parent kills us. RAII removal happens on _exit via
         * the destructor? No - _exit skips destructors, so unlink explicitly.
         * Simpler: block on a pause() and let the parent's kill end us; the
         * parent then observes a dead pid and steals the lock, which is also
         * a path worth exercising. */
        const atperson::StateLock lock(dir);
        const std::filesystem::path marker = dir / "child-ready";
        std::ofstream(marker).close();
        ::pause();
        /* Not reached. */
        _exit(0);
    }

    /* Wait for the child to hold the lock. */
    const std::filesystem::path marker = dir / "child-ready";
    for (int i = 0; i < 200 && !std::filesystem::exists(marker); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(std::filesystem::exists(marker));

    /* The live child owns the lock: the parent must be refused. */
    assert(acquire_fails(dir));

    /* End the child. */
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);

    /* The child was SIGKILLed, so its lockfile remains but the pid is dead:
     * stale detection must steal it. */
    const atperson::StateLock parent_lock(dir);
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_acquire_release_round_trip();
    test_second_acquire_fails();
    test_lock_file_records_owner();
    test_stale_dead_pid_is_stolen();
    test_malformed_lock_is_stale();
    test_empty_lock_is_graced_then_stolen();
    test_describe_lock();
    test_competing_processes();

    std::cout << "lock tests passed\n";
    return 0;
}
