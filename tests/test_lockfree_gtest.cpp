/**
 * test_lockfree_gtest.cpp — Lock-free queue tests ported to GTest
 */
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "lockfree_queue.h"
#include "threadpool_v2.h"

using namespace std::chrono_literals;

TEST(LockFreeQueueTest, StartsEmpty) {
    LockFreeQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
}

TEST(LockFreeQueueTest, BasicEnqueueDequeue) {
    LockFreeQueue<int, 8> q;
    EXPECT_TRUE(q.try_enqueue(42));
    EXPECT_FALSE(q.empty());
    auto val = q.try_dequeue();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeQueueTest, FIFOOrdering) {
    LockFreeQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) q.try_enqueue(i);
    for (int i = 0; i < 10; ++i) {
        auto v = q.try_dequeue();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i) << "FIFO violated at position " << i;
    }
}

TEST(LockFreeQueueTest, BoundedCapacityRejectsWhenFull) {
    LockFreeQueue<int, 4> q;
    int filled = 0;
    while (q.try_enqueue(filled)) ++filled;
    EXPECT_GT(filled, 0);
    EXPECT_FALSE(q.try_enqueue(999)) << "Queue must reject when full (backpressure)";
}

TEST(LockFreeQueueTest, RingBufferWrapAround) {
    LockFreeQueue<int, 4> q;
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(q.try_enqueue(i * 10));
        for (int i = 0; i < 3; ++i) {
            auto v = q.try_dequeue();
            ASSERT_TRUE(v.has_value()) << "cycle=" << cycle << " i=" << i;
            EXPECT_EQ(*v, i * 10);
        }
    }
}

TEST(LockFreeQueueTest, EmptyDequeueReturnsNullopt) {
    LockFreeQueue<int, 8> q;
    auto v = q.try_dequeue();
    EXPECT_FALSE(v.has_value());
}

TEST(LockFreeQueueTest, MPMCStressTest) {
    // 4 producers × 4 consumers, 40,000 items total
    // Every item produced must be consumed exactly once
    constexpr size_t CAPACITY  = 1024;
    constexpr int    PRODUCERS = 4;
    constexpr int    CONSUMERS = 4;
    constexpr int    PER_PROD  = 10000;
    constexpr int    TOTAL     = PRODUCERS * PER_PROD;

    LockFreeQueue<int, CAPACITY> q;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto& s : seen) s.store(0);

    std::vector<std::thread> threads;

    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&, p]{
            int base = p * PER_PROD;
            for (int i = 0; i < PER_PROD; ++i) {
                while (!q.try_enqueue(base + i))
                    std::this_thread::yield();
                ++produced;
            }
        });
    }

    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&]{
            while (consumed.load() < TOTAL) {
                if (auto v = q.try_dequeue()) {
                    ++seen[*v];
                    ++consumed;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(produced, TOTAL) << "Not all items were produced";
    EXPECT_EQ(consumed, TOTAL) << "Not all items were consumed";

    for (int i = 0; i < TOTAL; ++i) {
        EXPECT_EQ(seen[i], 1) << "Item " << i << " received " << seen[i] << " times";
    }
}

// ─────────────────────────────────────────────────────────────
// ThreadPoolV2 tests (lock-free pool without instrumentation)
// ─────────────────────────────────────────────────────────────
TEST(ThreadPoolV2Test, AllTasksExecute) {
    ThreadPoolV2<1024> pool(4);
    std::atomic<int> count{0};
    for (int i = 0; i < 1000; ++i)
        pool.enqueue([&count]{ ++count; });
    pool.wait_all();
    EXPECT_EQ(count, 1000);
}

TEST(ThreadPoolV2Test, FuturesReturnValues) {
    ThreadPoolV2<> pool(2);
    auto f1 = pool.enqueue([]{ return 99; });
    auto f2 = pool.enqueue([](int x){ return x * 2; }, 21);
    EXPECT_EQ(f1.get(), 99);
    EXPECT_EQ(f2.get(), 42);
}

TEST(ThreadPoolV2Test, MetricsAfterCompletion) {
    ThreadPoolV2<512> pool(2);
    for (int i = 0; i < 50; ++i)
        pool.enqueue([]{ std::this_thread::sleep_for(1ms); });

    EXPECT_EQ(pool.total_enqueued(), 50u);
    pool.wait_all();
    EXPECT_EQ(pool.total_completed(), 50u);
    EXPECT_EQ(pool.queue_depth(), 0u);
}
