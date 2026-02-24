/**
 * server.cpp — Distributed Task Server demo
 * ==========================================
 *
 * Starts a TaskServer on :8080 and a MetricsServer on :9090.
 * Clients connect over TCP, send task payloads, get results back.
 *
 * Run this, then in another terminal:
 *   ./client                          # run the client
 *   curl http://localhost:9090/metrics # see live metrics
 *   curl http://localhost:9090/health  # liveness probe
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>

#include "task_server.h"
#include "metrics_server.h"

using namespace std::chrono_literals;

int main() {
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Distributed Task Server — ThreadPool v4\n";
    std::cout << "═══════════════════════════════════════════════════\n\n";

    // Shared metrics registry — both servers write here,
    // MetricsServer exposes all of it on /metrics
    MetricsRegistry registry;

    // ── TASK HANDLER ──────────────────────────────────────────
    // This is what the server does with each incoming task.
    // In a real system: query a DB, call another service, compute something.
    // Here: echo back the input with some processing metadata.
    auto handler = [](const std::string& input) -> std::string {
        // Simulate variable work duration based on input length
        auto duration_ms = std::min(static_cast<int>(input.size() * 2), 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

        // Simulate 5% error rate
        if (input.find("fail") != std::string::npos)
            throw std::runtime_error("task explicitly requested failure");

        std::ostringstream result;
        result << "processed: [" << input << "] "
               << "len=" << input.size() << " "
               << "duration=" << duration_ms << "ms";
        return result.str();
    };

    // ── START TASK SERVER on :8080 ────────────────────────────
    TaskServer task_server(8080, handler, registry, 4);
    task_server.start();
    std::cout << "✓ Task server    → localhost:8080\n";

    // ── START METRICS SERVER on :9090 ────────────────────────
    MetricsServer metrics_server(registry, 9090);
    try {
        metrics_server.start();
        std::cout << "✓ Metrics server → http://localhost:9090/metrics\n";
        std::cout << "✓ Health probe   → http://localhost:9090/health\n";
    } catch (const std::exception& e) {
        std::cout << "⚠ Metrics server failed: " << e.what() << "\n";
    }

    std::cout << "\nWaiting for clients... (Ctrl+C to stop)\n";
    std::cout << "Run './client' in another terminal to test.\n\n";

    // Keep running until interrupted
    while (true) {
        std::this_thread::sleep_for(5s);

        // Print a live snapshot every 5 seconds
        std::string m = registry.serialize();
        auto extract = [&](const std::string& key) -> std::string {
            size_t pos = m.find('\n' + key + ' ');
            if (pos == std::string::npos)
                pos = m.find(key + ' ');
            if (pos == std::string::npos) return "?";
            size_t start = m.find(' ', pos) + 1;
            size_t end   = m.find('\n', start);
            return m.substr(start, end - start);
        };

        std::cout << "[snapshot] "
                  << "requests=" << extract("server_requests_total")
                  << " errors="  << extract("server_request_errors_total")
                  << " active_conns=" << extract("server_connections_active_current")
                  << " pool_completed=" << extract("threadpool_tasks_completed_total")
                  << "\n";
    }

    return 0;
}
