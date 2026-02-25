/**
 * test_client_server.cpp — Integration tests for TaskServer + TaskClient
 *
 * These tests start a real TCP server on localhost, connect a client,
 * and verify end-to-end behavior. Uses loopback (127.0.0.1) so no
 * network required — works in CI without any special setup.
 *
 * PORT STRATEGY:
 * Each test passes port=0 to TaskServer, which tells the OS to assign
 * a free ephemeral port automatically. After start(), server->port()
 * returns the actual port. This eliminates all port conflicts when
 * tests run in parallel across jobs or within the same binary.
 */
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <stdexcept>

#include "task_server.h"
#include "task_client.h"

using namespace std::chrono_literals;

// Helper: start server on a free OS-assigned port, connect client, tear down
class ServerClientFixture : public ::testing::Test {
protected:
    void SetUp() override {
        registry = std::make_unique<MetricsRegistry>();
    }

    void start_server(TaskServer::Handler handler) {
        // Port 0 → OS picks a free ephemeral port, no conflicts in parallel runs
        server = std::make_unique<TaskServer>(0, std::move(handler), *registry, 2);
        server->start();
        std::this_thread::sleep_for(50ms);  // let server finish binding
    }

    void connect_client() {
        // Read back the actual port the OS assigned
        client = std::make_unique<TaskClient>("127.0.0.1", server->port());
        client->connect();
    }

    void TearDown() override {
        if (client) client->disconnect();
        if (server) server->stop();
    }

    std::unique_ptr<MetricsRegistry> registry;
    std::unique_ptr<TaskServer>      server;
    std::unique_ptr<TaskClient>      client;
};

TEST_F(ServerClientFixture, PingPong) {
    start_server([](const std::string&){ return std::string("ok"); });
    connect_client();
    EXPECT_TRUE(client->ping());
}

TEST_F(ServerClientFixture, BasicSubmitAndReceive) {
    start_server([](const std::string& in){
        return std::string("echo: ") + in;
    });
    connect_client();

    auto f = client->submit("hello");
    EXPECT_EQ(f.get(), "echo: hello");
}

TEST_F(ServerClientFixture, MultipleRequests) {
    start_server([](const std::string& in){ return in + "_done"; });
    connect_client();

    for (int i = 0; i < 20; ++i) {
        auto f = client->submit("task" + std::to_string(i));
        EXPECT_EQ(f.get(), "task" + std::to_string(i) + "_done");
    }
}

TEST_F(ServerClientFixture, ServerErrorPropagatedToClient) {
    start_server([](const std::string&) -> std::string {
        throw std::runtime_error("deliberate server error");
    });
    connect_client();

    auto f = client->submit("anything");
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST_F(ServerClientFixture, LargePayload) {
    start_server([](const std::string& in){
        return std::string("size=") + std::to_string(in.size());
    });
    connect_client();

    std::string big(32 * 1024, 'Z');  // 32KB
    auto f = client->submit(big);
    EXPECT_EQ(f.get(), "size=32768");
}

TEST_F(ServerClientFixture, MetricsUpdatedAfterRequests) {
    start_server([](const std::string& in){ return in; });
    connect_client();

    for (int i = 0; i < 5; ++i) {
        auto f = client->submit("x");
        f.get();
    }

    std::string m = registry->serialize();
    EXPECT_NE(m.find("server_requests_total 5"), std::string::npos);
    EXPECT_NE(m.find("server_connections_accepted_total 1"), std::string::npos);
}

TEST_F(ServerClientFixture, ConcurrentClients) {
    start_server([](const std::string& in){ return in + "_ok"; });

    // 4 clients connect simultaneously
    constexpr int NUM_CLIENTS = 4;
    constexpr int TASKS_EACH  = 10;
    std::atomic<int> total_done{0};

    const int actual_port = server->port();  // capture before threads start

    std::vector<std::thread> threads;
    for (int c = 0; c < NUM_CLIENTS; ++c) {
        threads.emplace_back([&, c]{
            TaskClient cl("127.0.0.1", actual_port);
            cl.connect();
            for (int i = 0; i < TASKS_EACH; ++i) {
                std::string payload = "client" + std::to_string(c)
                                    + "-task" + std::to_string(i);
                auto f = cl.submit(payload);
                if (f.get() == payload + "_ok") ++total_done;
            }
            cl.disconnect();
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(total_done, NUM_CLIENTS * TASKS_EACH);
}
