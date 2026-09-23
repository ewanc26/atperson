#ifndef ATPERSON_WORKER_POOL_HPP
#define ATPERSON_WORKER_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace atperson {

struct SystemResources;

namespace worker_pool {

/* Implementation detail. Defined in pool.cpp; the public WorkerPool holds an
 * opaque pointer to it. Not copyable or movable. */
struct Impl;

} // namespace worker_pool

/* Bounded thread pool with one owner thread.

 * Workers pull tasks from a shared queue and run them concurrently. The pool
 * is fail-closed: if any worker task throws, the error is stored and re-thrown
 * by the next call to wait() or submit() that observes it. A throw leaves no
 * partial task state visible to the owner — the owner only ever sees complete
 * results or a stored error.

 * The pool has exactly one owner thread, which calls submit()/wait()/
 * shutdown(). Workers are internal and never touch shared mutable state the
 * owner reads; every result is handed back through a queue the owner drains.
 * This is the staged-batch boundary AGENTS.md requires before parallelising
 * ingestion: fetching may overlap, but every touch of a C23 core object goes
 * through the owner thread.

 * Not copyable or movable. */
class WorkerPool {
public:
    struct Config {
        std::size_t thread_count{1u}; /* >= 1 */
        std::size_t max_queue{64u};   /* backpressure bound */
    };

    explicit WorkerPool(Config config);
    ~WorkerPool();

    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;
    WorkerPool(WorkerPool &&) = delete;
    WorkerPool &operator=(WorkerPool &&) = delete;

    /* Submit one task tagged with a sequence number. The pool guarantees the
     * task runs, but not in what order relative to other submitted tasks.
     * Throws if the pool is shutting down or the queue is at its backpressure
     * bound (callers must wait() before submitting more). */
    void submit(std::size_t sequence, std::function<void()> task);

    /* Block until every submitted task has completed. Re-throws the first
     * stored worker error, if any. After wait() returns the pool is empty
     * and ready to accept new work. */
    void wait();

    /* Stop accepting new work, drop pending tasks, and join workers.
     * Idempotent. */
    void shutdown();

    /* Apply a new resource-derived configuration after all currently queued
     * work has completed. Only the owner thread may call this method. */
    void resize(Config config);

    std::size_t thread_count() const noexcept;
    std::size_t pending();
    bool shutting_down();

    /* Derive a pool config from system resources, with an operational cap.
     * effective_cpu_capacity is the absolute CPU count available after
     * container quotas/cpusets. A sub-one quota still gets one worker. A zero
     * max_threads means no additional cap. */
    static Config config_from_system(const SystemResources &system,
                                     std::size_t max_threads = 0u);

private:
    worker_pool::Impl *impl_;
};

} // namespace atperson

#endif
