#pragma once

#include "threadpool_v2.h"
#include "metrics.h"
#include <chrono>
#include <exception>

/**
 * ThreadPoolV3 — Lock-Free Thread Pool with Prometheus Observability
 * ===================================================================
 *
 * Wraps ThreadPoolV2 and adds instrumentation: every task submission,
 * completion, failure, queue depth, active worker count, and latency
 * is tracked and exposed in Prometheus text format.
 *
 * WAIT_ALL CORRECTNESS:
 * ---------------------
 * pool_.wait_all() (V2) unblocks when the V2-level active_tasks_ counter
 * hits zero. But V3 wraps each task in a lambda — the V3 metric updates
 * (tasks_completed_->inc(), task_latency_->observe_since()) happen INSIDE
 * that lambda. If those updates happen after V2's active_tasks_ decrement,
 * wait_all() can return before the metric counters are final.
 *
 * The fix: after pool_.wait_all(), spin until
 *   tasks_completed + tasks_failed == tasks_submitted
 * This is the only signal that ALL V3 bookkeeping is finished.
 */
template<size_t QueueCapacity = 1024>
class ThreadPoolV3 {
public:
    explicit ThreadPoolV3(
        size_t num_threads = std::thread::hardware_concurrency(),
        MetricsRegistry* registry = nullptr)
        : pool_(num_threads)
    {
        if (!registry) {
            private_registry_ = std::make_unique<MetricsRegistry>();
            registry = private_registry_.get();
        }

        tasks_submitted_ = registry->add_counter(
            "threadpool_tasks_submitted_total",
            "Total number of tasks submitted to the thread pool");
        tasks_completed_ = registry->add_counter(
            "threadpool_tasks_completed_total",
            "Total number of tasks that completed successfully");
        tasks_failed_ = registry->add_counter(
            "threadpool_tasks_failed_total",
            "Total number of tasks that threw an exception");
        queue_depth_ = registry->add_gauge(
            "threadpool_queue_depth_current",
            "Current number of tasks waiting in the queue");
        active_workers_ = registry->add_gauge(
            "threadpool_active_workers_current",
            "Current number of threads actively executing tasks");
        thread_count_ = registry->add_gauge(
            "threadpool_thread_count",
            "Total number of worker threads in the pool");
        thread_count_->set(static_cast<int64_t>(num_threads));
        task_latency_ = registry->add_histogram(
            "threadpool_task_latency_seconds",
            "End-to-end task latency from submission to completion");
    }

    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using R = typename std::invoke_result<F, Args...>::type;

        auto submit_time = std::chrono::steady_clock::now();
        tasks_submitted_->inc();

        auto prom = std::make_shared<std::promise<R>>();
        auto future = prom->get_future();
        auto fn = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        auto wrapper = [this, prom, fn=std::move(fn), submit_time]() mutable {
            active_workers_->inc();
            queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));

            bool ok = true;
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    prom->set_value();
                } else {
                    prom->set_value(fn());
                }
            } catch (...) {
                prom->set_exception(std::current_exception());
                tasks_failed_->inc();
                ok = false;
            }

            // Update metrics BEFORE decrementing active_workers_.
            // wait_all() polls tasks_completed+tasks_failed==tasks_submitted
            // so these must be committed before we signal "done".
            task_latency_->observe_since(submit_time);
            if (ok) tasks_completed_->inc();

            active_workers_->dec();
            queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));
        };

        pool_.enqueue(std::move(wrapper));
        queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));
        return future;
    }

    /**
     * wait_all — block until every submitted task has fully finished,
     * including all metric updates.
     *
     * Two-phase wait:
     *   Phase 1: pool_.wait_all() — waits until V2's execution queue
     *            is drained and no V2 worker is active.
     *   Phase 2: spin until tasks_completed + tasks_failed == tasks_submitted
     *            This catches the narrow window where a V2 worker has
     *            finished executing the wrapper but V3's metric increments
     *            (tasks_completed_->inc()) haven't landed yet.
     *
     * Without phase 2, a test reading tasks_completed() immediately after
     * wait_all() can observe a count that is off by 1 or more.
     */
    void wait_all() {
        pool_.wait_all();

        // Phase 2: ensure all V3 metric bookkeeping is complete.
        size_t submitted = tasks_submitted_->get();
        while (tasks_completed_->get() + tasks_failed_->get() < submitted) {
            std::this_thread::yield();
        }

        queue_depth_->set(0);
        active_workers_->set(0);
    }

    size_t tasks_submitted()  const { return tasks_submitted_->get(); }
    size_t tasks_completed()  const { return tasks_completed_->get(); }
    size_t tasks_failed()     const { return tasks_failed_->get(); }
    size_t queue_depth()      const { return pool_.queue_depth(); }
    size_t active_workers()   const { return pool_.active_count(); }
    size_t thread_count()     const { return pool_.thread_count(); }

    ~ThreadPoolV3() = default;
    ThreadPoolV3(const ThreadPoolV3&) = delete;
    ThreadPoolV3& operator=(const ThreadPoolV3&) = delete;

private:
    ThreadPoolV2<QueueCapacity>      pool_;
    std::unique_ptr<MetricsRegistry> private_registry_;

    Counter*   tasks_submitted_{nullptr};
    Counter*   tasks_completed_{nullptr};
    Counter*   tasks_failed_{nullptr};
    Gauge*     queue_depth_{nullptr};
    Gauge*     active_workers_{nullptr};
    Gauge*     thread_count_{nullptr};
    Histogram* task_latency_{nullptr};
};
