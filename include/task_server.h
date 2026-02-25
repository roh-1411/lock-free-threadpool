#pragma once

/**
 * task_server.h — TCP Task Execution Server
 *
 * THREAD SAFETY FIXES:
 * - server_fd_ is std::atomic<int> — eliminates the TSan data race between
 *   stop() writing server_fd_=-1 on the main thread and accept_loop()
 *   reading server_fd_ on the accept thread.
 * - stop() uses exchange() to atomically grab and close the fd exactly once.
 * - accept_loop() loads server_fd_ atomically before each accept() call.
 *
 * PORT 0 SUPPORT:
 * - Pass port=0 to let the OS assign a free ephemeral port.
 * - After start(), call port() to get the actual assigned port.
 * - This is the correct approach for tests — no hardcoded ports, no conflicts.
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
        server_fd_.store(setup_socket(), std::memory_order_release);
        running_.store(true, std::memory_order_release);
        accept_thread_ = std::thread([this]{ accept_loop(); });
        std::cout << "[TaskServer] Listening on :" << port_ << "\n";
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        // exchange atomically grabs the fd and sets it to -1 in one operation,
        // preventing accept_loop() from using a closed fd.
        int fd = server_fd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    // Returns the actual bound port. When port=0 was passed, this returns
    // the OS-assigned ephemeral port — call this after start().
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
        addr.sin_port        = htons(port_);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("TaskServer: bind() failed on port "
                                     + std::to_string(port_));
        }

        // Read back OS-assigned port when port=0 was requested
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
            int sfd = server_fd_.load(std::memory_order_acquire);
            if (sfd < 0) break;

            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = ::accept(sfd,
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len);
            if (client_fd < 0) {
                if (!running_.load(std::memory_order_acquire)) break;
                continue;
            }

            conn_accepted_->inc();
            conn_active_->inc();

            pool_.enqueue([this, client_fd]{
                handle_connection(client_fd);
                ::close(client_fd);
                conn_active_->dec();
            });
        }
    }

    void handle_connection(int fd) {
        while (running_.load(std::memory_order_acquire)) {
            proto::Message req;
            if (!proto::recv_message(fd, req)) break;

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
    std::atomic<int>        server_fd_;    // atomic — eliminates TSan race with accept_loop
    std::thread             accept_thread_;

    Counter*   conn_accepted_{nullptr};
    Gauge*     conn_active_{nullptr};
    Counter*   requests_total_{nullptr};
    Counter*   request_errors_{nullptr};
    Histogram* request_latency_{nullptr};
};
