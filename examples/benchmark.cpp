/**
 * benchmark.cpp
 * -------------
 * Head-to-head benchmark: ThreadPool (mutex) vs ThreadPoolV2 (lock-free)
 *
 * Measures throughput (tasks/sec) under different contention levels:
 *   - Low  contention: tasks are slow (queue rarely full)
 *   - High contention: tasks are instant (queue always contested)
 *
 * This is how real performance engineers compare implementations.
 * The results explain why production systems (nginx, DPDK, Seastar)
 * use lock-free queues on hot paths.
 *
 * Compile:
 *   g++ -std=c++17 -O2 -pthread examples/benchmark.cpp -Iinclude -o benchmark
 * Run:
 *   ./benchmark
 */

#include <iostream>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include "threadpool.h"
#include "threadpool_v2.h"

// ---- Timer helper ----
struct Timer {
    std::chrono::high_resolution_clock::time_point start;
    Timer() : start(std::chrono::high_resolution_clock::now()) {}
    double ms() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }
    double sec() const { return ms() / 1000.0; }
};

// ---- Benchmark result ----
struct Result {
    std::string name;
    double      elapsed_ms;
    size_t      tasks;
    double      throughput() const { return tasks / (elapsed_ms / 1000.0); }
};

void print_result(const Result& r) {
    std::cout << std::left << std::setw(30) << r.name
              << std::right << std::setw(10) << std::fixed << std::setprecision(1)
              << r.elapsed_ms << " ms  |  "
              << std::setw(12) << std::setprecision(0)
              << r.throughput() << " tasks/sec\n";
}

void print_speedup(const Result& baseline, const Result& challenger) {
    double speedup = challenger.throughput() / baseline.throughput();
    std::cout << "  → Lock-free speedup: " << std::setprecision(2)
              << speedup << "x " << (speedup > 1.0 ? "FASTER ✓" : "slower") << "\n\n";
}

// ---- Benchmark runner ----
template<typename Pool>
Result run_bench(const std::string& name, Pool& pool, size_t num_tasks, int work_us) {
    std::atomic<size_t> done{0};
    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    Timer t;
    for (size_t i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.enqueue([work_us, &done] {
            if (work_us > 0) {
                // Simulate real work (spin for N microseconds)
                auto end = std::chrono::high_resolution_clock::now()
                         + std::chrono::microseconds(work_us);
                while (std::chrono::high_resolution_clock::now() < end)
                    ;  // spin
            }
            ++done;
        }));
    }
    for (auto& f : futures) f.get();
    double elapsed = t.ms();

    return { name, elapsed, num_tasks };
}

int main() {
    const int  THREADS    = 4;
    const int  NUM_TASKS  = 50000;

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║       ThreadPool v1 (mutex) vs v2 (lock-free) Benchmark  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
    std::cout << "Threads: " << THREADS << " | Tasks per run: " << NUM_TASKS << "\n\n";

    std::cout << std::string(70, '-') << "\n";
    std::cout << "SCENARIO 1: HIGH CONTENTION — tiny tasks (queue always hot)\n";
    std::cout << "            Workers finish instantly → constant mutex/CAS pressure\n";
    std::cout << std::string(70, '-') << "\n";
    {
        ThreadPool    v1(THREADS);
        ThreadPoolV2<65536> v2(THREADS);

        auto r1 = run_bench("v1  mutex+cv  (0µs tasks)", v1, NUM_TASKS, 0);
        auto r2 = run_bench("v2  lock-free (0µs tasks)", v2, NUM_TASKS, 0);
        print_result(r1);
        print_result(r2);
        print_speedup(r1, r2);
    }

    std::cout << std::string(70, '-') << "\n";
    std::cout << "SCENARIO 2: MEDIUM CONTENTION — 10µs tasks\n";
    std::cout << "            Workers busy but queue contested regularly\n";
    std::cout << std::string(70, '-') << "\n";
    {
        ThreadPool    v1(THREADS);
        ThreadPoolV2<> v2(THREADS);

        auto r1 = run_bench("v1  mutex+cv  (10µs tasks)", v1, NUM_TASKS / 10, 10);
        auto r2 = run_bench("v2  lock-free (10µs tasks)", v2, NUM_TASKS / 10, 10);
        print_result(r1);
        print_result(r2);
        print_speedup(r1, r2);
    }

    std::cout << std::string(70, '-') << "\n";
    std::cout << "SCENARIO 3: LOW CONTENTION — 500µs tasks\n";
    std::cout << "            Workers mostly busy. Queue rarely contested.\n";
    std::cout << "            (mutex+cv sleep is OK here — workers idle rarely)\n";
    std::cout << std::string(70, '-') << "\n";
    {
        ThreadPool    v1(THREADS);
        ThreadPoolV2<> v2(THREADS);

        auto r1 = run_bench("v1  mutex+cv  (500µs tasks)", v1, 200, 500);
        auto r2 = run_bench("v2  lock-free (500µs tasks)", v2, 200, 500);
        print_result(r1);
        print_result(r2);
        print_speedup(r1, r2);
    }

    std::cout << "INSIGHT:\n";
    std::cout << "  High contention → lock-free wins (no context switches)\n";
    std::cout << "  Low  contention → mutex wins or ties (sleeping is free)\n";
    std::cout << "  Production systems use BOTH depending on the hot path.\n";

    return 0;
}
