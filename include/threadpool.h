#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>
#include <atomic>

/**
 * ThreadPool - A beginner-to-intermediate C++ project
 *
 * Concepts covered:
 *  - std::thread creation and joining
 *  - std::mutex and std::unique_lock (mutual exclusion)
 *  - std::condition_variable (thread signaling)
 *  - std::future / std::promise (async results)
 *  - std::atomic (lock-free flags)
 *  - Lambda functions and std::function
 *  - Move semantics
 *
 * Relevant to:
 *  - EECS 348 (C++ fundamentals, modularity)
 *  - EECS 678 (OS: scheduling, synchronization, deadlocks)
 *  - SRE: thread pools underpin every production server (nginx, gRPC, etc.)
 */
class ThreadPool {
public:
    /**
     * Constructor: spawns `num_threads` worker threads.
     * Each worker loops, waiting for tasks to appear in the queue.
     *
     * @param num_threads  Number of worker threads (default: hardware concurrency)
     */
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false), active_tasks_(0)
    {
        if (num_threads == 0)
            throw std::invalid_argument("ThreadPool: need at least 1 thread");

        workers_.reserve(num_threads);

        for (size_t i = 0; i < num_threads; ++i) {
            // Each worker runs this lambda forever until stop_ is set
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;

                    {
                        // --- CRITICAL SECTION START ---
                        std::unique_lock<std::mutex> lock(queue_mutex_);

                        // Block here until: a task arrives OR pool is stopping
                        condition_.wait(lock, [this] {
                            return stop_ || !tasks_.empty();
                        });

                        // If stopping and no more work, exit
                        if (stop_ && tasks_.empty())
                            return;

                        // Grab the next task (FIFO)
                        task = std::move(tasks_.front());
                        tasks_.pop();
                        ++active_tasks_;
                        // --- CRITICAL SECTION END ---
                    }

                    // Execute the task OUTSIDE the lock
                    // (so other threads can grab tasks concurrently)
                    task();
                    --active_tasks_;

                    // Signal anyone waiting on wait_all()
                    finished_condition_.notify_all();
                }
            });
        }
    }

    /**
     * Enqueue a callable and return a future to its result.
     *
     * Usage:
     *   auto future = pool.enqueue([]{ return 42; });
     *   int result = future.get();  // blocks until done
     *
     * Template magic explained:
     *   F    = the callable type (lambda, function pointer, etc.)
     *   Args = argument types
     *   R    = return type (deduced automatically)
     */
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using R = typename std::invoke_result<F, Args...>::type;

        if (stop_)
            throw std::runtime_error("ThreadPool: enqueue on stopped pool");

        // Package the task so we can get a future back
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<R> result = task_ptr->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // Wrap the packaged_task in a void() lambda for the queue
            tasks_.emplace([task_ptr]() { (*task_ptr)(); });
        }

        // Wake one sleeping worker
        condition_.notify_one();
        return result;
    }

    /**
     * Block until all currently queued tasks finish.
     * Useful in tests and benchmarks.
     */
    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        finished_condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

    /**
     * How many threads are in the pool?
     */
    size_t size() const { return workers_.size(); }

    /**
     * How many tasks are waiting to run?
     */
    size_t queue_size() const {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    /**
     * How many tasks are currently executing?
     */
    size_t active_count() const { return active_tasks_.load(); }

    /**
     * Destructor: stop accepting work, drain the queue, join all threads.
     *
     * This is the "graceful shutdown" pattern — important in SRE
     * (you want in-flight work to finish, not be killed mid-execution).
     */
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        // Wake ALL workers so they can see stop_ == true and exit
        condition_.notify_all();

        for (std::thread& worker : workers_)
            worker.join();
    }

    // Non-copyable, non-movable (owning threads)
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    std::vector<std::thread>          workers_;       // Worker threads
    std::queue<std::function<void()>> tasks_;         // Task queue (FIFO)

    mutable std::mutex                queue_mutex_;   // Protects tasks_ and stop_
    std::condition_variable           condition_;     // Signals: new task or stop
    std::condition_variable           finished_condition_; // Signals: task done

    bool                              stop_;          // Shutdown flag
    std::atomic<size_t>               active_tasks_;  // Currently running tasks
};
