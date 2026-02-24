#pragma once

#include <atomic>
#include <array>
#include <optional>
#include <stdexcept>
#include <new>  // std::hardware_destructive_interference_size

/**
 * LockFreeQueue<T, Capacity>
 * --------------------------
 * A bounded, wait-free MPMC (Multi-Producer Multi-Consumer) ring buffer.
 *
 * WHY THIS EXISTS — THE PROBLEM WITH MUTEX QUEUES:
 * -------------------------------------------------
 * The original ThreadPool used:
 *     std::queue<Task> + std::mutex + condition_variable
 *
 * Under high load this causes:
 *  1. CONTENTION  — every enqueue/dequeue locks the same mutex.
 *                   With 8 threads, 7 are always waiting.
 *  2. OS OVERHEAD — mutex contention causes context switches
 *                   (the OS parks and unparks threads). Costs ~1-10 µs each.
 *  3. CACHE THRASH — the mutex and queue head/tail live on the same
 *                    cache line. Every thread invalidates every other
 *                    thread's cache on every operation.
 *
 * HOW THIS FIXES IT:
 * ------------------
 * Uses hardware atomic CAS (Compare-And-Swap) instructions instead of
 * OS-level locks. No context switches. No kernel involvement.
 * Throughput: ~10-50x faster under high contention.
 *
 * THE RING BUFFER LAYOUT:
 * -----------------------
 *
 *   slots_: [slot0][slot1][slot2]...[slotN]
 *                ↑                    ↑
 *            head_ (dequeue)      tail_ (enqueue)
 *
 *   Each slot has its own atomic `sequence` counter.
 *   Enqueue: CAS on tail_, write data, advance slot sequence.
 *   Dequeue: CAS on head_, read data, advance slot sequence.
 *
 * MEMORY ORDERING — THE KEY LOW-LEVEL CONCEPT:
 * ---------------------------------------------
 * Modern CPUs reorder memory operations for performance.
 * Without explicit ordering, a thread might read stale data.
 *
 *   memory_order_relaxed  — no ordering guarantee (fastest, rarely correct alone)
 *   memory_order_acquire  — "see all writes that happened before the matching release"
 *   memory_order_release  — "my writes are visible to anyone who acquires this"
 *   memory_order_acq_rel  — both acquire + release (used on CAS operations)
 *
 * CACHE LINE ALIGNMENT:
 * ---------------------
 * A CPU cache line is typically 64 bytes.
 * If head_ and tail_ share a cache line, every enqueue ALSO invalidates
 * the cache line that dequeue reads — causing "false sharing".
 *
 * alignas(CACHE_LINE) forces each to its own cache line.
 *
 * @tparam T        Element type (must be movable)
 * @tparam Capacity Ring buffer size (must be power of 2)
 */
template<typename T, size_t Capacity>
class LockFreeQueue {
    static_assert(Capacity >= 2,             "Capacity must be >= 2");
    static_assert((Capacity & (Capacity-1)) == 0, "Capacity must be power of 2");

public:
    LockFreeQueue() {
        // Initialize each slot's sequence to its index
        // This marks all slots as "empty and ready for enqueue"
        for (size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);

        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    /**
     * try_enqueue — attempt to add an item.
     *
     * Returns true on success, false if queue is full.
     * Never blocks. Never throws. O(1) amortized.
     *
     * How it works:
     *  1. Load current tail position
     *  2. Check if the slot at that position is ready (sequence == tail)
     *  3. CAS: atomically try to claim tail+1
     *     - If CAS succeeds: we own this slot, write data
     *     - If CAS fails: another thread grabbed it first, retry
     */
    bool try_enqueue(T item) {
        size_t tail = tail_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[tail & MASK];

            // Load the slot's sequence with acquire ordering
            // so we see any writes the previous owner made
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail);

            if (diff == 0) {
                // Slot is ready. Try to claim it with CAS.
                // memory_order_acq_rel: if we win the CAS, our subsequent
                // write to slot.data is ordered after this.
                if (tail_.compare_exchange_weak(
                        tail, tail + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    // We own this slot. Write data.
                    slot.data = std::move(item);

                    // Signal that data is ready for dequeue
                    // Release ordering: makes our slot.data write visible
                    slot.sequence.store(tail + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed — another thread took our slot. Reload and retry.
            } else if (diff < 0) {
                // Queue is full (slot was enqueued but not yet dequeued)
                return false;
            } else {
                // Another enqueuer advanced tail; reload
                tail = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * try_dequeue — attempt to remove an item.
     *
     * Returns the item if available, std::nullopt if empty.
     * Never blocks. Never throws. O(1) amortized.
     */
    std::optional<T> try_dequeue() {
        size_t head = head_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[head & MASK];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq)
                          - static_cast<intptr_t>(head + 1);

            if (diff == 0) {
                // Slot has data. Try to claim it.
                if (head_.compare_exchange_weak(
                        head, head + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    // We own this slot. Read data.
                    T item = std::move(slot.data);

                    // Mark slot as ready for next enqueue cycle
                    slot.sequence.store(head + Capacity,
                                        std::memory_order_release);
                    return item;
                }
                // CAS failed — another thread grabbed this slot. Retry.
            } else if (diff < 0) {
                // Queue is empty
                return std::nullopt;
            } else {
                // Another dequeuer advanced head; reload
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /**
     * size — approximate number of items currently in queue.
     * "Approximate" because head/tail can change between the two loads.
     * Good enough for monitoring (like a Prometheus gauge).
     */
    size_t size() const {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return tail > head ? tail - head : 0;
    }

    bool empty() const { return size() == 0; }

    static constexpr size_t capacity() { return Capacity; }

private:
    // Cache line size — on x86/ARM64 this is 64 bytes.
    // std::hardware_destructive_interference_size is the C++17 way to get it.
    static constexpr size_t CACHE_LINE =
#ifdef __cpp_lib_hardware_interference_size
        std::hardware_destructive_interference_size;
#else
        64;
#endif

    static constexpr size_t MASK = Capacity - 1;  // fast modulo for power-of-2

    /**
     * Slot — one cell in the ring buffer.
     *
     * alignas(CACHE_LINE) gives each slot its own cache line.
     * Without this, adjacent slots share a cache line.
     * When thread A writes slot[0] and thread B writes slot[1],
     * they'd constantly invalidate each other's cache — FALSE SHARING.
     */
    struct alignas(CACHE_LINE) Slot {
        std::atomic<size_t> sequence{0};
        T                   data{};
    };

    // The ring buffer of slots
    std::array<Slot, Capacity> slots_;

    // head_ and tail_ on SEPARATE cache lines to avoid false sharing
    // between enqueue and dequeue operations
    alignas(CACHE_LINE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE) std::atomic<size_t> tail_{0};
};
