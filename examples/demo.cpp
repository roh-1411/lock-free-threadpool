/**
 * demo.cpp — ThreadPoolV3 with live Prometheus metrics
 * =====================================================
 *
 * Run this, then in another terminal:
 *   curl http://localhost:9090/metrics
 *   curl http://localhost:9090/health
 *
 * You'll see real Prometheus output:
 *   threadpool_tasks_submitted_total 1000
 *   threadpool_tasks_completed_total 997
 *   threadpool_task_latency_seconds_bucket{le="0.001"} 823
 *   ...
 *
 * This is what you'd wire Prometheus (the time-series DB) to scrape.
 */

#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <stdexcept>

#include "threadpool_v3.h"
#include "metrics_server.h"

using namespace std::chrono_literals;

int main() {
    std::cout << "═══════════════════════════════════════════════\n";
    std::cout << "  ThreadPoolV3 — Prometheus Metrics Demo\n";
    std::cout << "═══════════════════════════════════════════════\n\n";

    // Create shared metrics registry
    MetricsRegistry registry;

    // Create instrumented pool: 4 workers, attached to registry
    ThreadPoolV3<1024> pool(4, &registry);

    // Start the HTTP /metrics server on port 9090
    MetricsServer server(registry, 9090);
    try {
        server.start();
        std::cout << "✓ Metrics server running at http://localhost:9090/metrics\n";
        std::cout << "  Try: curl http://localhost:9090/metrics\n";
        std::cout << "  Try: curl http://localhost:9090/health\n\n";
    } catch (const std::exception& e) {
        std::cout << "⚠ Metrics server failed to start: " << e.what() << "\n";
        std::cout << "  (continuing without HTTP endpoint)\n\n";
    }

    // Simulate a workload: tasks with random durations + occasional failures
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> duration_dist(0, 10);  // 0-10ms
    std::uniform_int_distribution<int> fail_dist(1, 20);      // 5% failure rate

    constexpr int NUM_TASKS = 500;
    std::cout << "Submitting " << NUM_TASKS << " tasks...\n";

    for (int i = 0; i < NUM_TASKS; ++i) {
        int ms       = duration_dist(rng);
        bool should_fail = (fail_dist(rng) == 1);

        pool.enqueue([ms, should_fail, i]() -> int {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (should_fail)
                throw std::runtime_error("task " + std::to_string(i) + " failed");
            return i * 2;
        });

        // Print a snapshot every 100 tasks
        if ((i + 1) % 100 == 0) {
            std::cout << "  [" << (i+1) << "/" << NUM_TASKS << "] "
                      << "submitted=" << pool.tasks_submitted()
                      << " completed=" << pool.tasks_completed()
                      << " failed=" << pool.tasks_failed()
                      << " queue_depth=" << pool.queue_depth()
                      << "\n";
        }
    }

    pool.wait_all();

    std::cout << "\n═══════════════════════════════════════════════\n";
    std::cout << "  Final Metrics\n";
    std::cout << "═══════════════════════════════════════════════\n";
    std::cout << "  Tasks submitted:  " << pool.tasks_submitted() << "\n";
    std::cout << "  Tasks completed:  " << pool.tasks_completed() << "\n";
    std::cout << "  Tasks failed:     " << pool.tasks_failed() << "\n";
    std::cout << "  Error rate:       "
              << (100.0 * pool.tasks_failed() / pool.tasks_submitted()) << "%\n";

    std::cout << "\n── Prometheus Output (raw /metrics) ──────────\n";
    std::cout << registry.serialize();

    std::cout << "\nPress Enter to stop the metrics server...";
    std::cin.get();

    server.stop();
    return 0;
}
