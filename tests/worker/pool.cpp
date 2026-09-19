/* Worker pool contract tests (#29 parallel sync).
 *
 * Everything here runs offline: the pool is a pure threading primitive, so
 * the tests cover fail-closed behaviour, backpressure, and shutdown without
 * any network or graph state. */
#include "worker/pool.hpp"
#include "resource/system.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void test_pool_runs_tasks() {
    atperson::WorkerPool pool({4u, 64u});
    std::atomic<std::size_t> counter{0u};
    for (std::size_t i = 0u; i < 10u; ++i) {
        pool.submit(i, [&counter]() { counter.fetch_add(1u, std::memory_order_relaxed); });
    }
    pool.wait();
    assert(counter.load(std::memory_order_relaxed) == 10u);
    assert(pool.pending() == 0u);
    pool.shutdown();
}

void test_pool_propagates_worker_error() {
    atperson::WorkerPool pool({2u, 64u});
    pool.submit(0u, []() { throw std::runtime_error("boom"); });
    bool caught = false;
    try {
        pool.wait();
    } catch (const std::runtime_error &error) {
        caught = true;
        assert(std::string(error.what()) == "boom");
    }
    assert(caught);
    pool.shutdown();
}

void test_pool_backpressure_blocks_submit() {
    /* One worker, tiny queue: submitting a task that blocks the worker
     * fills the queue, and a second submit must block until wait() drains
     * it. The pool is fail-closed, so a submit after shutdown throws. */
    atperson::WorkerPool pool({1u, 1u});
    std::atomic<bool> started{false};
    std::atomic<bool> released{false};
    std::atomic<bool> submit_blocked{false};
    pool.submit(0u, [&]() {
        started.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    /* Queue is now full; this submit blocks until the task completes. */
    std::thread submitter([&]() {
        submit_blocked.store(true, std::memory_order_release);
        pool.submit(1u, []() {});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(submit_blocked.load(std::memory_order_acquire));
    released.store(true, std::memory_order_release);
    submitter.join();
    pool.wait();
    pool.shutdown();
}

void test_pool_shutdown_after_submit_is_idempotent() {
    atperson::WorkerPool pool({2u, 64u});
    pool.submit(0u, []() {});
    pool.shutdown();
    pool.shutdown(); /* idempotent */
    bool rejected = false;
    try {
        pool.submit(1u, []() {});
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected);
}

void test_pool_config_from_system() {
    atperson::SystemResources resources;
    resources.host_logical_cpus = 8u;
    resources.effective_cpu_capacity = 0.5; /* half a CPU */
    const auto config = atperson::WorkerPool::config_from_system(resources, 0u);
    assert(config.thread_count == 1u);
    const auto capped = atperson::WorkerPool::config_from_system(resources, 2u);
    assert(capped.thread_count == 1u);

    resources.effective_cpu_capacity = 8.0;
    assert(atperson::WorkerPool::config_from_system(resources, 0u).thread_count == 8u);

    resources.effective_cpu_capacity = 16.0;
    assert(atperson::WorkerPool::config_from_system(resources, 4u).thread_count == 4u);
}

} // namespace

int main() {
    test_pool_runs_tasks();
    test_pool_propagates_worker_error();
    test_pool_backpressure_blocks_submit();
    test_pool_shutdown_after_submit_is_idempotent();
    test_pool_config_from_system();
    std::printf("worker pool tests passed\n");
    return 0;
}
