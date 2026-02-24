/**
 * client.cpp — Task Client demo
 * ==============================
 *
 * Connects to the TaskServer on :8080, submits tasks, prints results.
 * Shows latency, success rate, and throughput.
 *
 * Run server first:  ./server
 * Then run this:     ./client
 */

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <sstream>

#include "task_client.h"

using namespace std::chrono;

int main(int argc, char* argv[]) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    int         port = (argc > 2) ? std::stoi(argv[2]) : 8080;

    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << "  Task Client — connecting to " << host << ":" << port << "\n";
    std::cout << "═══════════════════════════════════════════════════\n\n";

    TaskClient client(host, port);

    try {
        client.connect();
        std::cout << "✓ Connected to server\n\n";
    } catch (const std::exception& e) {
        std::cerr << "✗ Connection failed: " << e.what() << "\n";
        std::cerr << "  Is the server running? Start it with: ./server\n";
        return 1;
    }

    // ── PING TEST ─────────────────────────────────────────────
    std::cout << "── Ping test ──────────────────────────────────\n";
    bool alive = client.ping();
    std::cout << "  Server alive: " << (alive ? "✓ yes" : "✗ no") << "\n\n";
    if (!alive) return 1;

    // ── BASIC SUBMIT ──────────────────────────────────────────
    std::cout << "── Basic submit ────────────────────────────────\n";
    auto f = client.submit("hello from client");
    std::cout << "  Result: " << f.get() << "\n\n";

    // ── ERROR HANDLING ────────────────────────────────────────
    std::cout << "── Error handling ──────────────────────────────\n";
    try {
        auto ferr = client.submit("please fail this task");
        ferr.get();
        std::cout << "  (unexpected success)\n";
    } catch (const std::exception& e) {
        std::cout << "  Server error caught correctly: " << e.what() << "\n";
    }
    std::cout << "\n";

    // ── THROUGHPUT BENCHMARK ──────────────────────────────────
    std::cout << "── Throughput benchmark (100 tasks) ────────────\n";
    constexpr int N = 100;
    std::vector<long long> latencies_us;
    latencies_us.reserve(N);

    int succeeded = 0, failed = 0;
    auto bench_start = steady_clock::now();

    for (int i = 0; i < N; ++i) {
        std::string payload = "task-" + std::to_string(i) + " data:" + std::string(i % 20, 'x');
        auto t0 = steady_clock::now();
        try {
            auto fut = client.submit(payload);
            fut.get();
            ++succeeded;
        } catch (...) {
            ++failed;
        }
        auto t1 = steady_clock::now();
        latencies_us.push_back(duration_cast<microseconds>(t1 - t0).count());
    }

    auto bench_end  = steady_clock::now();
    double total_ms = duration_cast<microseconds>(bench_end - bench_start).count() / 1000.0;

    // Compute percentiles
    std::sort(latencies_us.begin(), latencies_us.end());
    auto p50  = latencies_us[N * 50  / 100];
    auto p95  = latencies_us[N * 95  / 100];
    auto p99  = latencies_us[N * 99  / 100];
    double avg = std::accumulate(latencies_us.begin(), latencies_us.end(), 0LL) / (double)N;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Tasks:       " << N << " (" << succeeded << " ok, " << failed << " failed)\n";
    std::cout << "  Total time:  " << total_ms << " ms\n";
    std::cout << "  Throughput:  " << (N / (total_ms / 1000.0)) << " req/s\n";
    std::cout << "  Latency avg: " << avg         << " µs\n";
    std::cout << "  Latency p50: " << p50         << " µs\n";
    std::cout << "  Latency p95: " << p95         << " µs\n";
    std::cout << "  Latency p99: " << p99         << " µs\n";

    std::cout << "\n✓ Done. Check http://localhost:9090/metrics for server-side stats.\n";

    client.disconnect();
    return 0;
}
