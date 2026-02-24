/**
 * test_metrics.cpp — Google Test suite for metrics + instrumented pool
 * =====================================================================
 *
 * Google Test (GTest) conventions:
 *   TEST(SuiteName, TestName)  — standalone test
 *   TEST_F(FixtureName, TestName) — test using a fixture (shared setup/teardown)
 *   EXPECT_*  — non-fatal assertion (test continues on failure)
 *   ASSERT_*  — fatal assertion (test stops on failure)
 *
 * Why GTest over our custom CHECK() macro?
 *   - Industry standard (used at Google, Meta, most C++ shops)
 *   - Rich assertion output (shows actual vs expected values)
 *   - Test filtering: --gtest_filter=Metrics.*
 *   - XML output for CI: --gtest_output=xml:results.xml
 *   - Death tests, parameterized tests, test fixtures
 */

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <stdexcept>

#include "metrics.h"
#include "threadpool_v3.h"

using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────
// Counter Tests
// ─────────────────────────────────────────────────────────────
TEST(CounterTest, StartsAtZero) {
    Counter c("test_counter", "A test counter");
    EXPECT_EQ(c.get(), 0u);
}

TEST(CounterTest, IncrementByOne) {
    Counter c("test_counter", "A test counter");
    c.inc();
    c.inc();
    EXPECT_EQ(c.get(), 2u);
}

TEST(CounterTest, IncrementByDelta) {
    Counter c("test_counter", "A test counter");
    c.inc(100);
    EXPECT_EQ(c.get(), 100u);
}

TEST(CounterTest, SerializeFormat) {
    Counter c("tasks_total", "Total tasks");
    c.inc(42);
    std::string s = c.serialize();
    EXPECT_NE(s.find("# HELP tasks_total"), std::string::npos);
    EXPECT_NE(s.find("# TYPE tasks_total counter"), std::string::npos);
    EXPECT_NE(s.find("tasks_total 42"), std::string::npos);
}

TEST(CounterTest, ConcurrentIncrements) {
    // Counter must be thread-safe — test under concurrency
    Counter c("concurrent_counter", "For concurrent test");
    constexpr int THREADS = 8;
    constexpr int INCS_PER_THREAD = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&c]{ 
            for (int j = 0; j < INCS_PER_THREAD; ++j) c.inc(); 
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(c.get(), static_cast<uint64_t>(THREADS * INCS_PER_THREAD));
}

// ─────────────────────────────────────────────────────────────
// Gauge Tests
// ─────────────────────────────────────────────────────────────
TEST(GaugeTest, StartsAtZero) {
    Gauge g("queue_depth", "Current queue depth");
    EXPECT_EQ(g.get(), 0);
}

TEST(GaugeTest, SetAndGet) {
    Gauge g("active_workers", "Active workers");
    g.set(4);
    EXPECT_EQ(g.get(), 4);
    g.set(0);
    EXPECT_EQ(g.get(), 0);
}

TEST(GaugeTest, IncDec) {
    Gauge g("connections", "Active connections");
    g.inc();
    g.inc();
    g.inc();
    EXPECT_EQ(g.get(), 3);
    g.dec();
    EXPECT_EQ(g.get(), 2);
}

TEST(GaugeTest, SerializeFormat) {
    Gauge g("queue_depth_current", "Queue depth");
    g.set(7);
    std::string s = g.serialize();
    EXPECT_NE(s.find("# TYPE queue_depth_current gauge"), std::string::npos);
    EXPECT_NE(s.find("queue_depth_current 7"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// Histogram Tests
// ─────────────────────────────────────────────────────────────
TEST(HistogramTest, ObserveIncrementsBuckets) {
    Histogram h("latency", "Latency", {0.001, 0.01, 0.1});
    h.observe(0.005);  // falls in 0.01 and 0.1 buckets, not 0.001

    std::string s = h.serialize();
    EXPECT_NE(s.find("latency_count 1"), std::string::npos);
}

TEST(HistogramTest, SerializeContainsBuckets) {
    Histogram h("latency_seconds", "Task latency", {0.001, 0.01});
    h.observe(0.0005);  // below 0.001 bucket
    h.observe(0.005);   // above 0.001, below 0.01

    std::string s = h.serialize();
    EXPECT_NE(s.find("latency_seconds_bucket"), std::string::npos);
    EXPECT_NE(s.find("latency_seconds_sum"),    std::string::npos);
    EXPECT_NE(s.find("latency_seconds_count 2"), std::string::npos);
    EXPECT_NE(s.find("+Inf"), std::string::npos);
}

TEST(HistogramTest, ObserveSince) {
    Histogram h("latency", "Latency");
    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1ms);
    h.observe_since(start);

    std::string s = h.serialize();
    EXPECT_NE(s.find("latency_count 1"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// MetricsRegistry Tests
// ─────────────────────────────────────────────────────────────
TEST(RegistryTest, SerializeContainsAllMetrics) {
    MetricsRegistry reg;
    auto* c = reg.add_counter("req_total", "Requests");
    auto* g = reg.add_gauge("active", "Active connections");
    auto* h = reg.add_histogram("latency_seconds", "Latency");

    c->inc(5);
    g->set(3);
    h->observe(0.001);

    std::string s = reg.serialize();
    EXPECT_NE(s.find("req_total 5"), std::string::npos);
    EXPECT_NE(s.find("active 3"), std::string::npos);
    EXPECT_NE(s.find("latency_seconds_count 1"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────
// ThreadPoolV3 Tests
// ─────────────────────────────────────────────────────────────
class PoolFixture : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<ThreadPoolV3<1024>>(4, &registry);
    }

    MetricsRegistry registry;
    std::unique_ptr<ThreadPoolV3<1024>> pool;
};

TEST_F(PoolFixture, TasksSubmittedCounterIncreases) {
    for (int i = 0; i < 10; ++i)
        pool->enqueue([]{ return 0; });
    pool->wait_all();
    EXPECT_EQ(pool->tasks_submitted(), 10u);
}

TEST_F(PoolFixture, TasksCompletedCounterIncreases) {
    for (int i = 0; i < 20; ++i)
        pool->enqueue([]{ return 0; });
    pool->wait_all();
    EXPECT_EQ(pool->tasks_completed(), 20u);
}

TEST_F(PoolFixture, FailedTasksCounter) {
    // Tasks that throw should increment failed counter
    for (int i = 0; i < 5; ++i) {
        pool->enqueue([]() -> int { throw std::runtime_error("intentional"); });
    }
    // Also submit 5 that succeed
    for (int i = 0; i < 5; ++i) {
        pool->enqueue([]{ return 42; });
    }
    pool->wait_all();

    EXPECT_EQ(pool->tasks_failed(), 5u);
    EXPECT_EQ(pool->tasks_completed(), 5u);
    EXPECT_EQ(pool->tasks_submitted(), 10u);
}

TEST_F(PoolFixture, FuturesReturnCorrectValues) {
    auto f1 = pool->enqueue([]{ return 99; });
    auto f2 = pool->enqueue([](int x){ return x * 2; }, 21);

    EXPECT_EQ(f1.get(), 99);
    EXPECT_EQ(f2.get(), 42);
}

TEST_F(PoolFixture, MetricsSerializeToPrometheusFormat) {
    for (int i = 0; i < 3; ++i)
        pool->enqueue([]{ std::this_thread::sleep_for(1ms); });
    pool->wait_all();

    std::string metrics = registry.serialize();

    // Must contain all Four Golden Signals
    EXPECT_NE(metrics.find("threadpool_tasks_submitted_total"), std::string::npos);
    EXPECT_NE(metrics.find("threadpool_tasks_completed_total"), std::string::npos);
    EXPECT_NE(metrics.find("threadpool_tasks_failed_total"),    std::string::npos);
    EXPECT_NE(metrics.find("threadpool_queue_depth_current"),   std::string::npos);
    EXPECT_NE(metrics.find("threadpool_active_workers_current"), std::string::npos);
    EXPECT_NE(metrics.find("threadpool_task_latency_seconds"),  std::string::npos);
    // Histogram must have bucket, sum, count
    EXPECT_NE(metrics.find("_bucket{le="), std::string::npos);
    EXPECT_NE(metrics.find("_sum"), std::string::npos);
    EXPECT_NE(metrics.find("_count 3"), std::string::npos);
}

TEST_F(PoolFixture, ThreadCountGaugeIsCorrect) {
    std::string metrics = registry.serialize();
    EXPECT_NE(metrics.find("threadpool_thread_count 4"), std::string::npos);
}
