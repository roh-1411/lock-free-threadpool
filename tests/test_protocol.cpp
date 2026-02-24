/**
 * test_protocol.cpp — GTest suite for wire protocol
 */
#include <gtest/gtest.h>
#include "protocol.h"

TEST(ProtocolTest, EncodeDecodeRoundtrip) {
    // Create a socket pair for testing (no network needed)
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    proto::Message sent(proto::MessageType::REQUEST, 42, std::string("hello world"));
    ASSERT_TRUE(proto::send_message(sv[0], sent));

    proto::Message received;
    ASSERT_TRUE(proto::recv_message(sv[1], received));

    EXPECT_EQ(static_cast<int>(received.type), static_cast<int>(proto::MessageType::REQUEST));
    EXPECT_EQ(received.id, 42u);
    EXPECT_EQ(received.payload_str(), "hello world");

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(ProtocolTest, EmptyPayload) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    proto::Message sent(proto::MessageType::PING, 1, std::string(""));
    ASSERT_TRUE(proto::send_message(sv[0], sent));

    proto::Message received;
    ASSERT_TRUE(proto::recv_message(sv[1], received));

    EXPECT_EQ(static_cast<int>(received.type), static_cast<int>(proto::MessageType::PING));
    EXPECT_EQ(received.id, 1u);
    EXPECT_TRUE(received.payload.empty());

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(ProtocolTest, LargePayload) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    std::string big(64 * 1024, 'A');  // 64KB payload
    proto::Message sent(proto::MessageType::REQUEST, 99, big);
    ASSERT_TRUE(proto::send_message(sv[0], sent));

    proto::Message received;
    ASSERT_TRUE(proto::recv_message(sv[1], received));

    EXPECT_EQ(received.payload_str(), big);
    EXPECT_EQ(received.id, 99u);

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(ProtocolTest, MultipleMessages) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    // Send 3 messages back to back
    for (int i = 0; i < 3; ++i) {
        proto::Message m(proto::MessageType::REQUEST, i,
                         "message-" + std::to_string(i));
        ASSERT_TRUE(proto::send_message(sv[0], m));
    }

    // Receive all 3 in order
    for (int i = 0; i < 3; ++i) {
        proto::Message m;
        ASSERT_TRUE(proto::recv_message(sv[1], m));
        EXPECT_EQ(m.id, static_cast<uint32_t>(i));
        EXPECT_EQ(m.payload_str(), "message-" + std::to_string(i));
    }

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(ProtocolTest, AllMessageTypes) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    std::vector<proto::MessageType> types = {
        proto::MessageType::REQUEST,
        proto::MessageType::RESPONSE,
        proto::MessageType::ERROR,
        proto::MessageType::PING,
        proto::MessageType::PONG,
    };

    for (auto t : types) {
        proto::Message sent(t, 0, std::string("data"));
        ASSERT_TRUE(proto::send_message(sv[0], sent));
        proto::Message recv;
        ASSERT_TRUE(proto::recv_message(sv[1], recv));
        EXPECT_EQ(static_cast<int>(recv.type), static_cast<int>(t));
    }

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST(ProtocolTest, ClosedSocketReturnsFalse) {
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ::close(sv[1]);  // close receiver

    proto::Message sent(proto::MessageType::REQUEST, 1, std::string("data"));
    // send may or may not fail immediately depending on kernel buffer
    // but recv on a closed socket must fail
    proto::Message recv;
    bool result = proto::recv_message(sv[0], recv);
    EXPECT_FALSE(result);

    ::close(sv[0]);
}
