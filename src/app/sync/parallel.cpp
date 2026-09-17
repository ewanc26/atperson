#include "parallel.hpp"

#include "system.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace atperson {
namespace {

/* A fetched page, handed back from a worker to the owner. `sequence` is the
 * submission order; the owner drains in sequence order. */
struct PendingPage {
    std::size_t sequence;
    SyncPage page;
};

} // namespace

SyncResult run_sync_parallel(LanguageGraph &graph, Ledger &ledger, IngestionState &state,
                            const SyncPageFetcher &fetch_page, const SyncLimits &limits,
                            WorkerPool &pool, const SyncLinker &link) {
    if (limits.page_size <= 0 || limits.max_pages <= 0) {
        throw std::runtime_error("sync limits must be positive");
    }

    SyncResult result;
    bool fresh_traversal = !state.catchup.active;

    if (fresh_traversal) {
        state.checkpoint.pages_completed = 0u;
        state.checkpoint.observations_seen = 0u;
    }

    std::optional<std::string> cursor =
        state.catchup.active ? state.catchup.cursor : std::optional<std::string>{};

    /* Owner-side queue of pages fetched out of order. Drained in sequence
     * order so the owner processes observations in cursor order. */
    std::deque<PendingPage> ready;
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    std::atomic<bool> fetch_error{false};

    std::size_t submitted = 0u;
    std::size_t consumed = 0u;
    bool fetch_exhausted = false;

    /* Fetch one page through the pool. The worker only does network I/O; it
     * never touches the graph, the ledger, or the ingestion state. */
    auto submit_fetch = [&](const std::optional<std::string> &page_cursor) {
        const std::size_t sequence = submitted++;
        pool.submit(sequence, [&, page_cursor, sequence]() {
            try {
                const SyncPage page = fetch_page(page_cursor);
                std::unique_lock<std::mutex> lock(ready_mutex);
                ready.push_back(PendingPage{sequence, std::move(page)});
            } catch (...) {
                fetch_error.store(true, std::memory_order_release);
            }
            ready_cv.notify_one();
        });
    };

    /* The owner drains one page at a time in sequence order. If the next
     * page has not been fetched yet, the owner blocks on the condition
     * variable — which is the point: while the owner waits, a worker is
     * fetching. */
    auto drain_next = [&]() -> SyncPage {
        std::unique_lock<std::mutex> lock(ready_mutex);
        while (true) {
            if (fetch_error.load(std::memory_order_acquire)) {
                throw std::runtime_error(
                    "sync aborted: a page fetch failed; cursor and ledger untouched");
            }
            if (!ready.empty() && ready.front().sequence == consumed) {
                SyncPage page = std::move(ready.front().page);
                ready.pop_front();
                ++consumed;
                return page;
            }
            if (fetch_exhausted && ready.empty()) {
                throw std::runtime_error("sync exhausted: no more pages");
            }
            ready_cv.wait(lock);
        }
    };

    int pages = 0;
    while (pages < limits.max_pages) {
        /* Submit the next fetch before draining so the worker has something to
         * do while the owner processes the current page. The pool's
         * backpressure bound bounds outstanding fetches. */
        if (!fetch_exhausted) {
            try {
                submit_fetch(cursor);
            } catch (const std::exception &error) {
                /* Pool full or shutting down: stop submitting and drain what
                 * is already in flight. */
                fetch_exhausted = true;
            }
        }

        const SyncPage page = drain_next();
        ++pages;

        for (const auto &observation : page.items) {
            ++result.observations_seen;
            if (process_observation(graph, ledger, observation)) {
                if (link) {
                    link(observation);
                }
                const bool trainable = !observation.text.empty();
                const bool policy_skipped =
                    observation.policy_reason != PolicyReason::Eligible &&
                    observation.policy_reason != PolicyReason::Repost &&
                    observation.policy_reason != PolicyReason::Reply;
                if (trainable && !policy_skipped) {
                    result.learned++;
                } else {
                    result.skipped++;
                }
            } else {
                result.duplicates++;
            }
        }

        result.pages_completed++;

        if (!page.next_cursor) {
            result.exhausted = true;
            break;
        }

        if (result.observations_seen >= limits.max_observations &&
            limits.max_observations > 0u) {
            cursor = page.next_cursor;
            break;
        }

        cursor = page.next_cursor;
    }

    /* Drain any outstanding fetches before checkpointing: the cursor must
     * never advance past observations that have not been durably handled, and
     * an in-flight fetch cannot be cancelled cleanly. */
    pool.wait();

    if (result.exhausted) {
        state.catchup.active = false;
        state.catchup.cursor = std::nullopt;
    } else {
        state.catchup.active = true;
        state.catchup.cursor = cursor;
    }
    state.checkpoint.pages_completed += result.pages_completed;
    state.checkpoint.observations_seen += result.observations_seen;
    return result;
}

} // namespace atperson