#pragma once

/**
 * metrics_server.h — Minimal HTTP server for /metrics endpoint
 * =============================================================
 *
 * Implements a single-purpose HTTP/1.1 server that responds to:
 *   GET /metrics  → Prometheus text format
 *   GET /health   → "OK" (liveness probe, standard in Kubernetes/GCP)
 *   GET /         → redirect hint
 *
 * WHY A CUSTOM HTTP SERVER?
 * --------------------------
 * Production systems use full libraries (Beast, Pistache, cpp-httplib).
 * This is intentionally minimal (~150 lines) to be:
 *   1. Zero external dependencies — compiles anywhere with POSIX
 *   2. Transparent — every line is readable and explainable
 *   3. Interview-ready — you can walk through each TCP concept
 *
 * TCP CONCEPTS DEMONSTRATED:
 * --------------------------
 *   socket()     → create a file descriptor for network I/O
 *   setsockopt() → set SO_REUSEADDR (allows restart without "address in use")
 *   bind()       → associate socket with a port
 *   listen()     → mark socket as passive (accept connections)
 *   accept()     → block until a client connects, return new fd
 *   recv()       → read bytes from client (the HTTP request)
 *   send()       → write bytes to client (the HTTP response)
 *   close()      → release the file descriptor
 *
 * This is how every HTTP server works at the bottom, whether it's
 * nginx, Apache, or a multi-billion request/day Google frontend.
 *
 * THREADING MODEL:
 * ----------------
 * The server runs in a background std::thread.
 * Each connection is handled inline (no thread per connection).
 * This is fine for a metrics endpoint with rare scrapes (~every 15s).
 * For production: use the thread pool to handle connections.
 */

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <cstring>

// POSIX sockets — Linux/macOS
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "metrics.h"

class MetricsServer {
public:
    /**
     * @param registry  The metrics registry to serve
     * @param port      Port to listen on (default: 9090, Prometheus convention)
     */
    explicit MetricsServer(MetricsRegistry& registry, int port = 9090)
        : registry_(registry)
        , port_(port)
        , running_(false)
        , server_fd_(-1)
    {}

    /**
     * start() — begin serving in a background thread.
     * Non-blocking. Call stop() to shut down.
     */
    void start() {
        running_.store(true, std::memory_order_release);
        server_fd_ = setup_socket();
        thread_ = std::thread([this]{ serve_loop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (server_fd_ >= 0) {
            ::shutdown(server_fd_, SHUT_RDWR);
            ::close(server_fd_);
            server_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }

    ~MetricsServer() {
        if (running_) stop();
    }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

private:
    int setup_socket() {
        // AF_INET = IPv4, SOCK_STREAM = TCP
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::runtime_error("MetricsServer: socket() failed");

        // SO_REUSEADDR: allow rebinding immediately after process restart
        // Without this: "Address already in use" for ~60 seconds after crash
        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;   // listen on all interfaces
        addr.sin_port        = htons(port_); // host-to-network byte order

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            throw std::runtime_error("MetricsServer: bind() failed on port "
                                     + std::to_string(port_));
        }

        // SOMAXCONN = max pending connections in the kernel accept queue
        if (::listen(fd, SOMAXCONN) < 0) {
            ::close(fd);
            throw std::runtime_error("MetricsServer: listen() failed");
        }

        return fd;
    }

    void serve_loop() {
        while (running_.load(std::memory_order_acquire)) {
            // accept() blocks until a client connects
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_,
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len);

            if (client_fd < 0) {
                if (!running_) break;  // graceful shutdown
                continue;
            }

            handle_connection(client_fd);
            ::close(client_fd);
        }
    }

    void handle_connection(int client_fd) {
        // Read the HTTP request (we only care about the first line)
        char buf[1024] = {};
        ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return;

        std::string request(buf, n);
        std::string response;

        if (request.rfind("GET /metrics", 0) == 0) {
            // Prometheus scrape endpoint
            std::string body = registry_.serialize();
            response = http_response("200 OK", "text/plain; version=0.0.4", body);
        } else if (request.rfind("GET /health", 0) == 0) {
            // Kubernetes liveness probe — always 200 if process is alive
            response = http_response("200 OK", "text/plain", "OK\n");
        } else {
            // Minimal 404 with hint
            response = http_response("404 Not Found", "text/plain",
                "Endpoints: /metrics, /health\n");
        }

        ::send(client_fd, response.c_str(), response.size(), 0);
    }

    static std::string http_response(const std::string& status,
                                     const std::string& content_type,
                                     const std::string& body) {
        std::ostringstream ss;
        ss << "HTTP/1.1 " << status << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        return ss.str();
    }

    MetricsRegistry& registry_;
    int              port_;
    std::atomic<bool> running_;
    int              server_fd_;
    std::thread      thread_;
};
