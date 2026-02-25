#pragma once

/**
 * task_server.h — TCP Task Execution Server
 * ==========================================
 *
 * WHAT THIS DOES:
 * ---------------
 * Listens on a TCP port. When a client connects and sends a REQUEST
 * message, the server submits the task to the thread pool, waits for
 * the result, and sends a RESPONSE back.
 *
 * This makes the thread pool accessible to ANY program on the network —
 * not just code running in the same process.
 *
 * ARCHITECTURE — how it handles multiple clients:
 * ------------------------------------------------
 * The server uses the thread pool to handle connections.
 * Each accepted connection becomes a task in the pool.
 *
 *   accept() → new client fd
 *       ↓
 *   pool.enqueue([fd]{ handle_connection(fd); })
 *       ↓
 *   worker thread reads request → executes → sends response
 *
 * This is the same model used by:
 *   - Apache httpd (thread-per-request)
 *   - Java's ThreadPoolExecutor in servlet containers
 *   - Early versions of nginx before event-loop model
 *
 * The key insight: the pool that processes tasks IS ALSO the pool
 * that handles network connections. The server is self-contained.
 *
 * TASK HANDLER:
 * -------------
 * The server doesn't know what tasks mean. It accepts a
 * std::function<std::string(std::string)> as the task handler.
 * The caller decides what to do with the payload and what to return.
 *
 * This is the Strategy pattern — behavior injected at construction time.
 *
 *   TaskServer server(8080, [](std::string input) -> std::string {
 *       // Do anything here: compute, query DB, call another service
 *       return "result: " + input;
 *   });
 *
 * PORT 0 SUPPORT:
 * ---------------
 * Passing port=0 lets the OS assign a free ephemeral port automatically.
 * After start(), call port() to find out which port was assigned.
 * This is the recommended approach for tests — it eliminates all port
 * conflicts when tests run in parallel.
 *
 *   TaskServer server(0, handler, registry);  // OS picks a free port
 *   server.start();
 *   int actual_port = server.port();          // read it back
 *
 * METRICS INTEGRATION:
 * --------------------
 * The server registers its own metrics in the shared registry:
 *   connections_accepted_total   — Counter
 *   connections_active_current   — Gauge
 *   request_latency_seconds      — Histogram (end-to-end per request)
 *   requests_total               — Counter
 *   request_errors_total         — Counter
 *
 * These appear alongside the pool metrics on /metrics — giving a
 * complete picture of the service from both the network and compute layer.
 */

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <iostream>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "threadpool_v3.h"
#include "metrics.h"
#include "protocol.h"

class TaskServer {
public:
    using Handler = std::function<std::string(const std::string&)>;

    /**
     * @param port      TCP port to listen on (pass 0 to let OS pick a free port)
     * @param handler   Function to run for each incoming task
     * @param registry  Metrics registry (shared with MetricsServer)
     * @param threads   Worker thread count
     */
    TaskServer(int port,
               Handler handler,
               MetricsRegistry& registry,
               size_t threads = std::thread::hardware_concurrency())
        : port_(port)
        , handler_(std::move(handler))
        , pool_(threads, &registry)
        , running_(false)
        , server_fd_(-1)
    {
        // Register network-layer metrics
        conn_accepted_  = registry.add_counter(
            "server_connections_accepted_total",
            "Total TCP connections accepted");

        conn_active_    = registry.add_gauge(
            "server_connections_active_current",
            "Currently open TCP connections");

        requests_total_ = registry.add_counter(
            "server_requests_total",
            "Total task requests received");

        request_errors_ = registry.add_counter(
            "server_request_errors_total",
            "Total requests that resulted in errors");

        request_latency_ = registry.add_histogram(
            "server_request_latency_seconds",
            "End-to-end request latency from TCP receive to TCP send");
    }

    void start() {
        server_fd_ = setup_socket();  // port_ updated here if port=0 was passed
        running_.store(true, std::memory_order_release);
        accept_thread_ = std::thread([this]{ accept_loop(); });
        std::cout << "[TaskServer] Listening on :" << port_ << "\n";
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (server_fd_ >= 0) {
            ::shutdown(server_fd_, SHUT_RDWR);
            ::close(server_fd_);
            server_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    // Returns the actual port — useful when port=0 was passed and the OS
    // assigned an ephemeral port. Call this after start().
    int port() const { return port_; }

    ~TaskServer() { if (running_) stop(); }

    TaskServer(const TaskServer&) = delete;
    TaskServer& operator=(const TaskServer&) = delete;

private:
    int setup_socket() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("TaskServer: socket() failed");

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);  // htons(0) = let OS pick

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("TaskServer: bind() failed on port "
                                     + std::to_string(port_));
        }

        // If port 0 was requested, read back the OS-assigned port
        if (port_ == 0) {
            sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
                port_ = ntohs(bound.sin_port);
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            ::close(fd);
            throw std::runtime_error("TaskServer: listen() failed");
        }
        return fd;
    }

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_,
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len);
            if (client_fd < 0) {
                if (!running_) break;
                continue;
            }

            conn_accepted_->inc();
            conn_active_->inc();

            // Hand the connection to the thread pool
            // The pool worker handles the full request/response cycle
            pool_.enqueue([this, client_fd]{
                handle_connection(client_fd);
                ::close(client_fd);
                conn_active_->dec();
            });
        }
    }

    void handle_connection(int fd) {
        // A single connection can send multiple requests (pipelining)
        while (running_.load(std::memory_order_acquire)) {
            proto::Message req;
            if (!proto::recv_message(fd, req)) break;  // client disconnected

            // Handle PING — liveness check, reply immediately
            if (req.type == proto::MessageType::PING) {
                proto::Message pong(proto::MessageType::PONG, req.id, "");
                proto::send_message(fd, pong);
                continue;
            }

            if (req.type != proto::MessageType::REQUEST) break;

            auto start = std::chrono::steady_clock::now();
            requests_total_->inc();

            std::string result;
            proto::MessageType resp_type = proto::MessageType::RESPONSE;

            try {
                result = handler_(req.payload_str());
            } catch (const std::exception& e) {
                result    = std::string("ERROR: ") + e.what();
                resp_type = proto::MessageType::ERROR;
                request_errors_->inc();
            } catch (...) {
                result    = "ERROR: unknown exception";
                resp_type = proto::MessageType::ERROR;
                request_errors_->inc();
            }

            proto::Message resp(resp_type, req.id, result);
            if (!proto::send_message(fd, resp)) break;

            request_latency_->observe_since(start);
        }
    }

    int                     port_;
    Handler                 handler_;
    ThreadPoolV3<1024>      pool_;
    std::atomic<bool>       running_;
    int                     server_fd_;
    std::thread             accept_thread_;

    // Metrics
    Counter*   conn_accepted_{nullptr};
    Gauge*     conn_active_{nullptr};
    Counter*   requests_total_{nullptr};
    Counter*   request_errors_{nullptr};
    Histogram* request_latency_{nullptr};
};
