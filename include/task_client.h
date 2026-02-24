#pragma once

/**
 * task_client.h — TCP Task Client
 * =================================
 *
 * WHAT THIS DOES:
 * ---------------
 * Connects to a TaskServer over TCP and submits tasks remotely.
 * Returns std::future<std::string> — same interface as the local pool.
 * The caller doesn't need to know whether execution is local or remote.
 *
 * This is the "location transparency" principle in distributed systems:
 * code that submits work shouldn't care where the work runs.
 *
 * CONNECTION MODEL:
 * -----------------
 * One persistent TCP connection per client instance.
 * Requests are sent sequentially on that connection.
 *
 * For a production system you'd use a connection pool
 * (multiple connections, round-robin). This is the single-connection
 * version — correct, simple, and sufficient for a portfolio project.
 *
 * REQUEST ID:
 * -----------
 * Each request gets a unique uint32 ID (atomic counter).
 * The server echoes this ID back in the response.
 * This lets us match responses to requests — critical for
 * async pipelining where responses may arrive out of order.
 *
 * USAGE:
 * ------
 *   TaskClient client("localhost", 8080);
 *   client.connect();
 *
 *   auto f1 = client.submit("hello");
 *   auto f2 = client.submit("world");
 *
 *   std::cout << f1.get() << "\n";  // blocks until server responds
 *   std::cout << f2.get() << "\n";
 *
 *   client.ping();  // check server is alive
 */

#include <string>
#include <future>
#include <stdexcept>
#include <atomic>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "protocol.h"

class TaskClient {
public:
    TaskClient(std::string host, int port)
        : host_(std::move(host))
        , port_(port)
        , fd_(-1)
        , next_id_(1)
        , connected_(false)
    {}

    /**
     * connect() — establish TCP connection to the server.
     * Call this before submit().
     */
    void connect() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("TaskClient: socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);

        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("TaskClient: invalid address: " + host_);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("TaskClient: connect() failed to "
                                     + host_ + ":" + std::to_string(port_));

        connected_ = true;
    }

    /**
     * submit() — send a task to the server, return a future.
     *
     * This is synchronous (send → recv on the same connection).
     * The future is immediately resolved once the server responds.
     *
     * For truly async behavior you'd need a separate reader thread
     * and a map of id → promise. That's the next extension.
     */
    std::future<std::string> submit(const std::string& payload) {
        if (!connected_)
            throw std::runtime_error("TaskClient: not connected");

        uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        proto::Message req(proto::MessageType::REQUEST, id, payload);

        if (!proto::send_message(fd_, req))
            throw std::runtime_error("TaskClient: send failed");

        proto::Message resp;
        if (!proto::recv_message(fd_, resp))
            throw std::runtime_error("TaskClient: recv failed");

        std::promise<std::string> prom;

        if (resp.type == proto::MessageType::ERROR) {
            prom.set_exception(std::make_exception_ptr(
                std::runtime_error(resp.payload_str())));
        } else {
            prom.set_value(resp.payload_str());
        }

        return prom.get_future();
    }

    /**
     * ping() — check if server is alive.
     * Returns true if server responds with PONG within the connection timeout.
     */
    bool ping() {
        if (!connected_) return false;
        uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        proto::Message req(proto::MessageType::PING, id, "");
        if (!proto::send_message(fd_, req)) return false;
        proto::Message resp;
        if (!proto::recv_message(fd_, resp)) return false;
        return resp.type == proto::MessageType::PONG;
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    bool is_connected() const { return connected_; }

    ~TaskClient() { disconnect(); }

    TaskClient(const TaskClient&) = delete;
    TaskClient& operator=(const TaskClient&) = delete;

private:
    std::string          host_;
    int                  port_;
    int                  fd_;
    std::atomic<uint32_t> next_id_;
    bool                 connected_;
};
