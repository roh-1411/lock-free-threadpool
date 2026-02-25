#pragma once

#include "threadpool_v2.h"
#include "metrics.h"
#include <chrono>
#include <exception>

template<size_t QueueCapacity = 1024>
class ThreadPoolV3 {
public:
    /**
     * @param num_threads  Worker thread count (default: hardware_concurrency)
     * @param registry     Metrics registry to register our metrics with.
     *                     If nullptr, metrics are still collected but not exposed.
     */
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

    /**
     * enqueue — submit a task, returns a future.
     *
     * ORDERING FIX:
     * tasks_completed_->inc() and task_latency_->observe_since() are called
     * BEFORE active_workers_->dec(). This is critical for wait_all() correctness.
     *
     * wait_all() polls active_tasks_ (inside pool_.wait_all()) dropping to zero
     * as the signal that all work is done. If metric updates happened after the
     * decrement, wait_all() could return before tasks_completed_ was updated,
     * causing tests that read tasks_completed() immediately after wait_all()
     * to observe a stale (too-low) count.
     */
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
            // pool_.wait_all() returns when active_tasks_ hits zero, which
            // happens when threadpool_v2's worker calls fetch_sub on active_tasks_.
            // Any metric updates that happen after that decrement are invisible
            // to callers that read metrics immediately after wait_all().
            task_latency_->observe_since(submit_time);
            if (ok) tasks_completed_->inc();

            // Decrement last — this is what unblocks wait_all().
            active_workers_->dec();
            queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));
        };

        pool_.enqueue(std::move(wrapper));
        queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));
        return future;
    }

    void wait_all() {
        pool_.wait_all();
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
