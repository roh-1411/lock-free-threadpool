# lock-free-threadpool

![CI](https://github.com/YOUR_USERNAME/lock-free-threadpool/actions/workflows/ci.yml/badge.svg)

A production-grade C++17 lock-free thread pool with **Prometheus observability** — implementing Google SRE's Four Golden Signals via a live HTTP `/metrics` endpoint.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  ThreadPoolV3                       │
│  ┌──────────────────────────────────────────────┐  │
│  │  MetricsRegistry                             │  │
│  │   Counter   tasks_submitted_total            │  │
│  │   Counter   tasks_completed_total            │  │
│  │   Counter   tasks_failed_total               │  │
│  │   Gauge     queue_depth_current              │  │
│  │   Gauge     active_workers_current           │  │
│  │   Histogram task_latency_seconds (p99)       │  │
│  └──────────────────────────────────────────────┘  │
│                     ↓ wraps                         │
│  ┌──────────────────────────────────────────────┐  │
│  │  ThreadPoolV2 — lock-free MPMC queue         │  │
│  │  CAS ring buffer, no mutex, no syscalls      │  │
│  │  N worker threads (spin → yield strategy)   │  │
│  └──────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
         ↕ HTTP :9090
┌─────────────────────────────────────────────────────┐
│  MetricsServer (raw POSIX TCP)                      │
│   GET /metrics  → Prometheus text format            │
│   GET /health   → "OK" (Kubernetes liveness probe)  │
└─────────────────────────────────────────────────────┘
```

## Four Golden Signals (Google SRE)

| Signal | Metric | Type |
|---|---|---|
| **Latency** | `threadpool_task_latency_seconds` | Histogram |
| **Traffic** | `threadpool_tasks_submitted_total` | Counter |
| **Errors** | `threadpool_tasks_failed_total` | Counter |
| **Saturation** | `threadpool_queue_depth_current` | Gauge |

## Build

### CMake (recommended)
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure
```

### Make (quick)
```bash
make demo
./demo
curl http://localhost:9090/metrics
curl http://localhost:9090/health
```

### With sanitizers
```bash
# ThreadSanitizer — catches data races in lock-free code
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON

# AddressSanitizer + UBSan — memory errors
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
```

## Sample `/metrics` Output

```
# HELP threadpool_tasks_submitted_total Total number of tasks submitted
# TYPE threadpool_tasks_submitted_total counter
threadpool_tasks_submitted_total 12345

# HELP threadpool_queue_depth_current Current tasks waiting in queue
# TYPE threadpool_queue_depth_current gauge
threadpool_queue_depth_current 42

# HELP threadpool_task_latency_seconds End-to-end task latency
# TYPE threadpool_task_latency_seconds histogram
threadpool_task_latency_seconds_bucket{le="0.001"} 9823
threadpool_task_latency_seconds_bucket{le="0.01"}  12100
threadpool_task_latency_seconds_bucket{le="+Inf"}  12345
threadpool_task_latency_seconds_sum   12.34
threadpool_task_latency_seconds_count 12345
```

## Key Techniques

| Technique | Where | Why |
|---|---|---|
| CAS ring buffer | `lockfree_queue.h` | No mutex contention, no OS involvement |
| `memory_order_acquire/release` | `lockfree_queue.h` | Explicit CPU reordering control |
| `alignas(64)` cache-line padding | `lockfree_queue.h` | Eliminates false sharing between head/tail |
| `__builtin_ia32_pause()` | `threadpool_v2.h` | Reduces spin-wait power + memory contention |
| `unique_ptr<atomic<uint64_t>[]>` | `metrics.h` | `std::atomic` is non-movable; fixed heap array sidesteps vector reallocation |
| Backpressure | `threadpool_v2.h` | Bounded queue returns false when full (Kafka/gRPC pattern) |
| `std::promise` over `packaged_task` | `threadpool_v3.h` | `packaged_task::operator()` silently stores exceptions; promise lets us intercept them |

## Performance

Under 8-thread contention (`-O2`, x86-64):

| | Mutex pool | Lock-free pool |
|---|---|---|
| p99 enqueue latency | ~18µs | ~3µs |
| Context switches | High | Near-zero |
| Best for | Infrequent tasks | High-throughput workloads |

## Testing

```bash
cd build
ctest --output-on-failure          # run all
ctest -R LockFree                  # just lock-free queue tests
ctest -R Metrics                   # just metrics tests

# With verbose GTest output:
./test_lockfree --gtest_filter=*
./test_metrics  --gtest_filter=PoolFixture.*
./test_metrics  --gtest_output=xml:results.xml
```

## File Structure

```
include/
  lockfree_queue.h    — Bounded MPMC ring buffer (CAS, cache-aligned)
  threadpool_v2.h     — Lock-free thread pool (spin → yield)
  threadpool_v3.h     — Instrumented wrapper (Prometheus metrics)
  metrics.h           — Counter / Gauge / Histogram / MetricsRegistry
  metrics_server.h    — TCP HTTP server for /metrics (raw POSIX sockets)
tests/
  test_lockfree_gtest.cpp  — 11 GTest tests (MPMC stress, FIFO, wrap-around)
  test_metrics.cpp         — 14 GTest tests (metrics + pool correctness)
examples/
  demo.cpp            — Live demo with /metrics HTTP endpoint
  benchmark.cpp       — Mutex vs lock-free latency comparison
.github/workflows/
  ci.yml              — Build + Test + TSan + ASan on every push
CMakeLists.txt        — CMake build (FetchContent GTest, optional sanitizers)
Makefile              — Quick build fallback
```
