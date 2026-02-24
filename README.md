# lock-free-threadpool

![CI](https://github.com/roh-1411/lock-free-threadpool/actions/workflows/ci.yml/badge.svg)

A distributed C++17 task execution server — lock-free thread pool with Prometheus observability and TCP networking.

## Architecture

```
  Client (any machine)
       │  TCP :8080
       ▼
  ┌─────────────────────────────────────────┐
  │  TaskServer                             │
  │  accept() → enqueue connection          │
  │       │                                 │
  │       ▼                                 │
  │  ┌─────────────────────────────────┐    │
  │  │  ThreadPoolV3 (lock-free MPMC)  │    │
  │  │  Counter / Gauge / Histogram    │    │
  │  └─────────────────────────────────┘    │
  │       │                                 │
  │       ▼                                 │
  │  MetricsServer  GET /metrics  :9090     │
  │  MetricsServer  GET /health   :9090     │
  └─────────────────────────────────────────┘
```

## Build & Run

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure

# Terminal 1 — start the server
./server

# Terminal 2 — run the client benchmark
./client

# Terminal 3 — scrape live Prometheus metrics
curl http://localhost:9090/metrics
curl http://localhost:9090/health
```

## What's in each file

```
include/
  lockfree_queue.h    — Bounded MPMC ring buffer (CAS, alignas(64))
  threadpool_v2.h     — Lock-free worker threads
  threadpool_v3.h     — Prometheus instrumentation layer
  metrics.h           — Counter / Gauge / Histogram / MetricsRegistry
  metrics_server.h    — HTTP /metrics endpoint (raw POSIX TCP)
  protocol.h          — Length-prefixed binary wire protocol
  task_server.h       — TCP task server (uses pool to handle connections)
  task_client.h       — TCP client with future-based API

tests/
  test_lockfree_gtest.cpp   — 11 tests: MPMC, FIFO, stress (40K items)
  test_metrics.cpp          — 14 tests: Counter/Gauge/Histogram/Pool
  test_protocol.cpp         — 6 tests: encode/decode, large payload, multi-message
  test_client_server.cpp    — 7 tests: ping, submit, errors, concurrent clients

examples/
  server.cpp    — starts TaskServer :8080 + MetricsServer :9090
  client.cpp    — connects, submits 100 tasks, prints p50/p95/p99 latency
  demo.cpp      — single-process demo with live /metrics
  benchmark.cpp — mutex vs lock-free latency comparison
```

## Prometheus output

```
curl http://localhost:9090/metrics

# Pool metrics (Four Golden Signals)
threadpool_tasks_submitted_total 500
threadpool_tasks_completed_total 475
threadpool_tasks_failed_total 25
threadpool_queue_depth_current 0
threadpool_task_latency_seconds_bucket{le="0.01"} 6
threadpool_task_latency_seconds_count 500

# Network metrics
server_requests_total 100
server_request_errors_total 0
server_connections_accepted_total 1
server_request_latency_seconds_count 100
```

## With sanitizers

```bash
cmake .. -DENABLE_TSAN=ON        # ThreadSanitizer
cmake .. -DENABLE_SANITIZERS=ON  # AddressSanitizer + UBSan
```
