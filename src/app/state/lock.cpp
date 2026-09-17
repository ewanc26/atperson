#include "lock.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace atperson {
namespace {

constexpr const char *kLockFileName = ".writer-lock";

/* Boot marker: a value that changes across reboots but is stable within one.
 * macOS: system boot time from sysctl kern.boottime. Linux: btime from
 * /proc/stat. Anything else: 0, which disables the reboot check (pid liveness
 * still guards correctness). */
std::string current_boot_marker() {
#if defined(__APPLE__)
    struct timeval boottime {};
    std::size_t size = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &size, nullptr, 0) == 0) {
        return "boot:" + std::to_string(static_cast<long long>(boottime.tv_sec));
    }
    return "boot:?";
#elif defined(__linux__)
    std::ifstream proc("/proc/stat");
    std::string key;
    while (proc >> key) {
        if (key == "btime") {
            long long btime = 0;
            if (proc >> btime) {
                return "boot:" + std::to_string(btime);
            }
            break;
        }
        /* Skip the rest of the line. */
        proc.ignore(4096, '\n');
    }
    return "boot:?";
#else
    return "boot:0";
#endif
}

bool process_alive(long long pid) {
    if (pid <= 0) {
        return false;
    }
    /* kill with signal 0 is the standard liveness probe; ESRCH means the
     * process does not exist. EPERM means it exists but belongs to another
     * user - still alive. */
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

std::string utc_now_rfc3339() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec);
    return buffer;
}

/* Lockfile format: one line "pid boot created", all fields present. */
struct LockContents {
    long long pid = 0;
    std::string boot;
    std::string created;
};

bool parse_lock_file(const std::filesystem::path &path, LockContents &out) {
    std::ifstream file(path);
    std::string pid_text;
    if (!(file >> pid_text)) {
        return false;
    }
    out.pid = std::strtoll(pid_text.c_str(), nullptr, 10);
    return static_cast<bool>(file >> out.boot >> out.created);
}

/* A lock is provably stale when its owner is dead, or when it was taken on a
 * previous boot (the machine rebooted, so no live process can still hold
 * it). A malformed lockfile is treated as stale: it cannot be attributed to
 * any live owner. An EMPTY lockfile is different: the owner is between
 * creating the file and writing its metadata, so it is treated as held (the
 * caller retries briefly before giving up). */
bool lock_is_stale(const LockContents &contents, const std::string &boot) {
    if (contents.pid <= 0 || contents.boot.empty()) {
        return true;
    }
    if (!process_alive(contents.pid)) {
        return true;
    }
    return contents.boot != boot && contents.boot != "boot:?" && boot != "boot:?";
}

void write_lock_file(int fd, const std::string &boot) {
    const std::string payload = std::to_string(static_cast<long long>(::getpid())) +
                                " " + boot + " " + utc_now_rfc3339() + "\n";
    const ssize_t written = ::write(fd, payload.data(), payload.size());
    if (written < 0 || static_cast<std::size_t>(written) != payload.size()) {
        /* Best effort; the lock is still exclusive even if the metadata is
         * truncated, and stale detection falls back to pid liveness. */
    }
}

} // namespace

StateLock::StateLock(const std::filesystem::path &state_directory)
    : StateLock(state_directory, kLockFileName) {}

StateLock::StateLock(const std::filesystem::path &state_directory, std::string lock_file_name) {
    if (lock_file_name.empty() || lock_file_name.find('/') != std::string::npos ||
        lock_file_name.find('\\') != std::string::npos) {
        throw StateLockError("invalid lock file name '" + lock_file_name + "'");
    }
    std::error_code ec;
    std::filesystem::create_directories(state_directory, ec);
    lock_path_ = state_directory / lock_file_name;

    const std::string boot = current_boot_marker();

    /* An empty lockfile means the owner created it but has not written its
     * metadata yet (the create/write window is microseconds). Retry for a
     * short grace period before concluding the owner died mid-write. */
    constexpr int kEmptyRetries = 50; /* ~50ms at 1ms per retry */
    int empty_retries = 0;

    for (;;) {
        const int fd = ::open(lock_path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            write_lock_file(fd, boot);
            ::close(fd);
            return;
        }
        if (errno != EEXIST) {
            throw StateLockError("cannot create lock file " + lock_path_.string() +
                                  ": " + std::strerror(errno));
        }

        /* Someone holds the lock. Classify it. */
        const bool file_empty = std::filesystem::file_size(lock_path_, ec) == 0 && !ec;
        LockContents contents;
        const bool parsed = !file_empty && parse_lock_file(lock_path_, contents);

        if (file_empty) {
            if (++empty_retries <= kEmptyRetries) {
                ::usleep(1000);
                continue;
            }
            /* The owner died between create and write; the empty file cannot
             * be attributed to anyone, so steal it. */
            std::filesystem::remove(lock_path_, ec);
            continue;
        }

        if (parsed && lock_is_stale(contents, boot)) {
            std::filesystem::remove(lock_path_, ec);
            continue; /* Retry the O_EXCL create. */
        }

        const std::string holder = parsed
                                       ? ("pid " + std::to_string(contents.pid) + " taken " +
                                          contents.created)
                                       : "an unparseable lock file";
        throw StateLockError("another atperson process owns " +
                             state_directory.string() + " (" + holder +
                             "); wait for it to finish or remove " +
                             lock_path_.string() + " if you are certain it is stale");
    }
}

StateLock::~StateLock() {
    if (!lock_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(lock_path_, ec);
    }
}

std::string describe_state_lock(const std::filesystem::path &state_directory) {
    const std::filesystem::path path = state_directory / kLockFileName;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    LockContents contents;
    if (!parse_lock_file(path, contents)) {
        return "lock held (unparseable lock file " + path.string() + ")";
    }
    return "lock held by pid " + std::to_string(contents.pid) + " since " +
           contents.created;
}

} // namespace atperson
