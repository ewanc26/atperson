#include "pool.hpp"

#include "system.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace atperson {
namespace worker_pool {

/* One submitted task. `sequence` is informational only: the pool does not
 * reorder tasks, and the owner drains results in submission order. */
struct WorkerTask {
    std::size_t sequence;
    std::function<void()> task;
};

struct Impl {
    WorkerPool::Config config;
    std::vector<std::thread> workers;
    std::mutex mutex_;
    std::condition_variable work_available;
    std::condition_variable space_available;
    std::condition_variable work_done;
    std::deque<WorkerTask> queue;
    std::exception_ptr error;
    std::string error_message;
    bool stop{false};
    bool shutting_down{false};
    std::size_t pending{0u};

    explicit Impl(const WorkerPool::Config &config_in) : config(config_in) {
        if (config.thread_count == 0u) {
            throw std::runtime_error("worker pool thread count must be >= 1");
        }
        if (config.max_queue == 0u) {
            throw std::runtime_error("worker pool max_queue must be >= 1");
        }
        workers.reserve(config.thread_count);
        for (std::size_t i = 0u; i < config.thread_count; ++i) {
            workers.emplace_back(&Impl::worker_loop, this);
        }
    }

    ~Impl() { shutdown(); }

    void submit(std::size_t sequence, std::function<void()> task) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutting_down) {
            throw std::runtime_error("worker pool is shutting down");
        }
        while (pending >= config.max_queue && !shutting_down && !error) {
            space_available.wait(lock);
        }
        if (shutting_down || error) {
            throw_first_error_locked();
        }
        queue.push_back(WorkerTask{sequence, std::move(task)});
        ++pending;
        work_available.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (pending > 0u && !error) {
            work_done.wait(lock);
        }
        throw_first_error_locked();
    }

    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            shutting_down = true;
            space_available.notify_all();
            work_available.notify_all();
        }
        for (std::thread &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    std::size_t thread_count() const noexcept { return config.thread_count; }

    std::size_t pending_count() {
        std::unique_lock<std::mutex> lock(mutex_);
        return pending;
    }

    bool shutting_down_flag() {
        std::unique_lock<std::mutex> lock(mutex_);
        return shutting_down;
    }

private:
    /* Record a worker failure. The failed task still owns its queue slot and
     * pending count, so the owner's wait() can complete and observe the
     * stored error; a blocked submitter can also proceed. The worker then
     * exits rather than picking up more work. */
    void mark_failed(std::string message) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!error) {
            error = std::current_exception();
            error_message = std::move(message);
            space_available.notify_all();
            work_available.notify_all();
        }
        --pending;
        space_available.notify_one();
        if (pending == 0u) {
            work_done.notify_all();
        }
    }

    void worker_loop() {
        for (;;) {
            WorkerTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                while (queue.empty() && !stop && !error && !shutting_down) {
                    work_available.wait(lock);
                }
                if (stop || error || shutting_down) {
                    return;
                }
                task = std::move(queue.front());
                queue.pop_front();
            }
            try {
                task.task();
            } catch (const std::exception &error_value) {
                std::string message = error_value.what();
                mark_failed(std::move(message));
                return;
            } catch (...) {
                mark_failed("worker task failed with a non-exception error");
                return;
            }
            {
                std::unique_lock<std::mutex> lock(mutex_);
                --pending;
                /* A completed task frees a queue slot, so a submitter blocked
                 * on backpressure can proceed. */
                space_available.notify_one();
                if (pending == 0u) {
                    work_done.notify_all();
                }
            }
        }
    }

    void throw_first_error_locked() {
        if (error) {
            std::string message = error_message;
            error = std::exception_ptr();
            error_message.clear();
            throw std::runtime_error(message.empty() ? "worker task failed" : message);
        }
    }
};

} // namespace worker_pool

WorkerPool::WorkerPool(Config config) : impl_(nullptr) {
    if (config.thread_count == 0u) {
        config.thread_count = 1u;
    }
    if (config.max_queue == 0u) {
        config.max_queue = 1u;
    }
    impl_ = new worker_pool::Impl(std::move(config));
}

WorkerPool::~WorkerPool() { delete static_cast<worker_pool::Impl *>(impl_); }

void WorkerPool::submit(std::size_t sequence, std::function<void()> task) {
    static_cast<worker_pool::Impl *>(impl_)->submit(sequence, std::move(task));
}

void WorkerPool::wait() { static_cast<worker_pool::Impl *>(impl_)->wait(); }

void WorkerPool::shutdown() { static_cast<worker_pool::Impl *>(impl_)->shutdown(); }

std::size_t WorkerPool::thread_count() const noexcept {
    return static_cast<worker_pool::Impl *>(impl_)->thread_count();
}

std::size_t WorkerPool::pending() {
    return static_cast<worker_pool::Impl *>(impl_)->pending_count();
}

bool WorkerPool::shutting_down() {
    return static_cast<worker_pool::Impl *>(impl_)->shutting_down_flag();
}

WorkerPool::Config WorkerPool::config_from_system(const SystemResources &system,
                                                  std::size_t max_threads) {
    Config config;
    /* Effective CPU capacity is a double in [0, 1] folded from host CPUs and
     * container quotas. Round down so a fractional quota (e.g. 0.5) yields
     * half the host's workers rather than rounding up to 1. */
    double capacity = system.effective_cpu_capacity;
    if (capacity <= 0.0) {
        capacity = 1.0;
    }
    std::size_t workers = static_cast<std::size_t>(system.host_logical_cpus * capacity);
    if (workers == 0u) {
        workers = 1u;
    }
    if (max_threads > 0u && workers > max_threads) {
        workers = max_threads;
    }
    config.thread_count = workers;
    config.max_queue = config.thread_count * 4u;
    if (config.max_queue < 16u) {
        config.max_queue = 16u;
    }
    return config;
}

} // namespace atperson