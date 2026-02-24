#pragma once

/**
 * threadpool_v3.h — Instrumented Thread Pool
 * ============================================
 *
 * Extends ThreadPoolV2 (lock-free MPMC queue) with full Prometheus
 * observability — the SRE "Four Golden Signals":
 *
 *   LATENCY     → task_latency_seconds histogram (p50/p99/p999)
 *   TRAFFIC     → tasks_submitted_total counter
 *   ERRORS      → tasks_failed_total counter
 *   SATURATION  → queue_depth_current gauge, active_workers_current gauge
 *
 * DESIGN DECISION — why wrap instead of modify?
 * ----------------------------------------------
 * ThreadPoolV2 is the lock-free core. We don't want to mix
 * observability concerns into the hot path. Instead, ThreadPoolV3
 * wraps the enqueue/execute boundary with timing and counters.
 *
 * This is the same pattern used in production systems:
 *   - nginx: ngx_http_log_module wraps the request handler
 *   - gRPC: interceptors wrap RPCs
 *   - Envoy: stats filter wraps every request
 *
 * HOW THE LATENCY TIMER WORKS:
 * ----------------------------
 * When a task is enqueued, we record a start timestamp.
 * We wrap the task in a lambda that:
 *   1. Runs the original task
 *   2. Computes elapsed time
 *   3. Calls histogram.observe(elapsed_seconds)
 *
 * This captures QUEUE WAIT TIME + EXECUTION TIME (end-to-end latency).
 * In a real system you might track these separately. Here we keep it
 * simple: total time from submit → complete.
 */

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
        // Register metrics — if no registry provided, create a private one
        if (!registry) {
            private_registry_ = std::make_unique<MetricsRegistry>();
            registry = private_registry_.get();
        }

        // Four Golden Signals
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
     * Wraps the task to:
     *   1. Record submission time
     *   2. Update queue depth gauge
     *   3. On execution: track active workers, measure latency, count errors
     */
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using R = typename std::invoke_result<F, Args...>::type;

        auto submit_time = std::chrono::steady_clock::now();
        tasks_submitted_->inc();

        // Use promise/future directly so we can intercept exceptions
        // (packaged_task stores exceptions silently in the future state)
        auto prom = std::make_shared<std::promise<R>>();
        auto future = prom->get_future();

        // Bind the user function eagerly (captures args by value)
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
            active_workers_->dec();
            queue_depth_->set(static_cast<int64_t>(pool_.queue_depth()));
            task_latency_->observe_since(submit_time);
            if (ok) tasks_completed_->inc();
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

    // Direct metric accessors (for testing without a registry)
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
    ThreadPoolV2<QueueCapacity>   pool_;
    std::unique_ptr<MetricsRegistry> private_registry_;

    // Metric pointers — owned by the registry, lifetime >= pool
    Counter*   tasks_submitted_{nullptr};
    Counter*   tasks_completed_{nullptr};
    Counter*   tasks_failed_{nullptr};
    Gauge*     queue_depth_{nullptr};
    Gauge*     active_workers_{nullptr};
    Gauge*     thread_count_{nullptr};
    Histogram* task_latency_{nullptr};
};
