#pragma once
/**
 * metrics.h — Prometheus-compatible metrics registry
 * ====================================================
 * Google's "Four Golden Signals": Latency, Traffic, Errors, Saturation
 *
 * Three metric types:
 *   COUNTER   — monotonically increasing (tasks completed, bytes sent)
 *   GAUGE     — can go up/down (queue depth, active connections)
 *   HISTOGRAM — latency percentiles (p50/p99/p999). Averages lie; percentiles don't.
 */

#include <atomic>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <memory>
#include <cstdint>

// ─────────────────────────────────────────────────────────────
// Counter — monotonically increasing uint64
// ─────────────────────────────────────────────────────────────
class Counter {
public:
    Counter(std::string name, std::string help)
        : name_(std::move(name)), help_(std::move(help)), value_(0) {}

    void inc(uint64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }
    uint64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
    std::string serialize() const {
        std::ostringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n"
           << "# TYPE " << name_ << " counter\n"
           << name_ << " " << get() << "\n";
        return ss.str();
    }
private:
    std::string           name_, help_;
    std::atomic<uint64_t> value_;
};

// ─────────────────────────────────────────────────────────────
// Gauge — can go up and down (lock-free via atomic int64)
// ─────────────────────────────────────────────────────────────
class Gauge {
public:
    Gauge(std::string name, std::string help)
        : name_(std::move(name)), help_(std::move(help)), value_(0) {}

    void set(int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void inc() noexcept { value_.fetch_add(1, std::memory_order_relaxed); }
    void dec() noexcept { value_.fetch_sub(1, std::memory_order_relaxed); }
    int64_t get() const noexcept { return value_.load(std::memory_order_relaxed); }

    std::string serialize() const {
        std::ostringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n"
           << "# TYPE " << name_ << " gauge\n"
           << name_ << " " << get() << "\n";
        return ss.str();
    }
private:
    std::string          name_, help_;
    std::atomic<int64_t> value_;
};

// ─────────────────────────────────────────────────────────────
// Histogram — latency distribution
//
// Uses unique_ptr<atomic<uint64_t>[]> because std::atomic is
// not movable/copyable, which makes vector<atomic> incompatible
// with reallocation. Fixed-size heap array sidesteps this entirely.
//
// WHY HISTOGRAMS MATTER FOR SRE:
// Google SRE mandates SLOs in percentiles, not averages.
// A p99 of 10s means 1 in 100 users waits 10 seconds — catastrophic
// even if the average looks healthy at ~100ms.
// ─────────────────────────────────────────────────────────────
class Histogram {
public:
    static std::vector<double> default_buckets() {
        return {0.0001, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0};
    }

    Histogram(std::string name, std::string help,
              std::vector<double> buckets = default_buckets())
        : name_(std::move(name))
        , help_(std::move(help))
        , buckets_(std::move(buckets))
        , num_buckets_(buckets_.size() + 1)  // +1 for +Inf
        , bucket_counts_(new std::atomic<uint64_t>[buckets_.size() + 1])
        , sum_(0.0)
        , count_(0)
    {
        std::sort(buckets_.begin(), buckets_.end());
        for (size_t i = 0; i < num_buckets_; ++i)
            bucket_counts_[i].store(0, std::memory_order_relaxed);
    }

    void observe(double seconds) {
        for (size_t i = 0; i < buckets_.size(); ++i)
            if (seconds <= buckets_[i])
                bucket_counts_[i].fetch_add(1, std::memory_order_relaxed);
        bucket_counts_[num_buckets_ - 1].fetch_add(1, std::memory_order_relaxed); // +Inf
        { std::lock_guard<std::mutex> lk(sum_mtx_); sum_ += seconds; }
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    void observe_since(std::chrono::steady_clock::time_point start) {
        observe(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count());
    }

    std::string serialize() const {
        std::ostringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n"
           << "# TYPE " << name_ << " histogram\n";
        for (size_t i = 0; i < buckets_.size(); ++i)
            ss << name_ << "_bucket{le=\"" << buckets_[i] << "\"} "
               << bucket_counts_[i].load(std::memory_order_relaxed) << "\n";
        ss << name_ << "_bucket{le=\"+Inf\"} "
           << bucket_counts_[num_buckets_-1].load(std::memory_order_relaxed) << "\n";
        double s; { std::lock_guard<std::mutex> lk(sum_mtx_); s = sum_; }
        ss << name_ << "_sum " << s << "\n"
           << name_ << "_count " << count_.load(std::memory_order_relaxed) << "\n";
        return ss.str();
    }

private:
    std::string                                    name_, help_;
    std::vector<double>                            buckets_;
    size_t                                         num_buckets_;
    std::unique_ptr<std::atomic<uint64_t>[]>       bucket_counts_;
    double                                         sum_;
    mutable std::mutex                             sum_mtx_;
    std::atomic<uint64_t>                          count_;
};

// ─────────────────────────────────────────────────────────────
// MetricsRegistry — owns all metrics, serializes /metrics page
// ─────────────────────────────────────────────────────────────
class MetricsRegistry {
public:
    Counter* add_counter(std::string name, std::string help) {
        std::lock_guard<std::mutex> lk(mtx_);
        counters_.push_back(std::make_unique<Counter>(std::move(name), std::move(help)));
        return counters_.back().get();
    }
    Gauge* add_gauge(std::string name, std::string help) {
        std::lock_guard<std::mutex> lk(mtx_);
        gauges_.push_back(std::make_unique<Gauge>(std::move(name), std::move(help)));
        return gauges_.back().get();
    }
    Histogram* add_histogram(std::string name, std::string help,
                             std::vector<double> buckets = Histogram::default_buckets()) {
        std::lock_guard<std::mutex> lk(mtx_);
        histograms_.push_back(std::make_unique<Histogram>(
            std::move(name), std::move(help), std::move(buckets)));
        return histograms_.back().get();
    }
    // Serialize all metrics — this is what /metrics HTTP endpoint returns
    std::string serialize() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ostringstream ss;
        for (const auto& c : counters_)   ss << c->serialize() << "\n";
        for (const auto& g : gauges_)     ss << g->serialize() << "\n";
        for (const auto& h : histograms_) ss << h->serialize() << "\n";
        return ss.str();
    }
private:
    mutable std::mutex                    mtx_;
    std::vector<std::unique_ptr<Counter>>   counters_;
    std::vector<std::unique_ptr<Gauge>>     gauges_;
    std::vector<std::unique_ptr<Histogram>> histograms_;
};
