#include "signals.hpp"

#include <cerrno>
#include <csignal>
#include <ctime>

namespace atperson {
namespace {

volatile std::sig_atomic_t g_shutdown = 0;

extern "C" void handle_shutdown_signal(int /*signum*/) {
    g_shutdown = 1;
}

} // namespace

void install_shutdown_signals() {
    g_shutdown = 0;
    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);
}

bool shutdown_requested() noexcept {
    return g_shutdown != 0;
}

bool sleep_until_interrupted(std::chrono::milliseconds duration) {
    if (shutdown_requested()) {
        return true;
    }
    if (duration.count() <= 0) {
        return false;
    }

    timespec remaining{};
    remaining.tv_sec = static_cast<time_t>(duration.count() / 1000);
    remaining.tv_nsec = static_cast<long>((duration.count() % 1000) * 1000000LL);

    while (nanosleep(&remaining, &remaining) == -1) {
        if (shutdown_requested()) {
            return true;
        }
        if (errno != EINTR) {
            /* Any other error aborts the wait; the caller re-observes
             * shutdown and the loop's own bounds handle the rest. */
            return false;
        }
    }
    return shutdown_requested();
}

} // namespace atperson
