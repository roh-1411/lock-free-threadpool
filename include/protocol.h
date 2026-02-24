#pragma once

/**
 * protocol.h — Wire protocol for the distributed task server
 * ===========================================================
 *
 * WHAT IS A WIRE PROTOCOL?
 * ------------------------
 * When two programs communicate over TCP, they send raw bytes.
 * A wire protocol defines what those bytes mean — how to encode
 * a request into bytes on one end and decode it back on the other.
 *
 * This is the same problem Redis, Kafka, gRPC, and HTTP all solve.
 * We implement the simplest correct approach: length-prefixed framing.
 *
 * LENGTH-PREFIXED FRAMING:
 * ------------------------
 * TCP is a stream protocol — it delivers a continuous river of bytes
 * with no built-in message boundaries. If you send "HELLO" and "WORLD",
 * the receiver might get "HEL", then "LOWO", then "RLD" — TCP can
 * split and merge writes arbitrarily.
 *
 * The fix: prefix every message with its length in bytes.
 *
 *   ┌─────────────────────────────────────────────────────┐
 *   │  4 bytes (uint32, big-endian)  │  N bytes payload   │
 *   │  message length = N            │  actual data        │
 *   └─────────────────────────────────────────────────────┘
 *
 * Receiver: read 4 bytes → get N → read exactly N bytes → done.
 * This is how Redis's RESP protocol and Kafka's wire format work.
 *
 * BIG-ENDIAN (network byte order):
 * ---------------------------------
 * Different CPUs store multi-byte integers differently:
 *   Little-endian (x86): least significant byte first
 *   Big-endian (network): most significant byte first
 *
 * Network protocols use big-endian by convention (RFC 1700).
 * htonl() = "host to network long" — converts to big-endian.
 * ntohl() = "network to host long" — converts back.
 *
 * MESSAGE TYPES:
 * --------------
 *   REQUEST:  client → server  "execute this task"
 *   RESPONSE: server → client  "here is the result"
 *   ERROR:    server → client  "something went wrong"
 *   PING:     client → server  "are you alive?"
 *   PONG:     server → client  "yes, I'm alive" (health check)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

// POSIX socket headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace proto {

// ─────────────────────────────────────────────────────────────
// Message types
// ─────────────────────────────────────────────────────────────
enum class MessageType : uint8_t {
    REQUEST  = 0x01,   // client → server: submit a task
    RESPONSE = 0x02,   // server → client: task result
    ERROR    = 0x03,   // server → client: error message
    PING     = 0x04,   // client → server: liveness check
    PONG     = 0x05,   // server → client: liveness reply
};

// ─────────────────────────────────────────────────────────────
// Message — the unit of communication
//
// Every message has:
//   type     — what kind of message this is
//   id       — request ID, echoed in the response so client
//              can match responses to requests (important for
//              async/pipelined requests)
//   payload  — the actual data (task input or result output)
// ─────────────────────────────────────────────────────────────
struct Message {
    MessageType       type;
    uint32_t          id;        // request ID
    std::vector<char> payload;   // task data or result

    Message() : type(MessageType::REQUEST), id(0) {}

    Message(MessageType t, uint32_t i, std::string data)
        : type(t), id(i), payload(data.begin(), data.end()) {}

    Message(MessageType t, uint32_t i, std::vector<char> data)
        : type(t), id(i), payload(std::move(data)) {}

    std::string payload_str() const {
        return std::string(payload.begin(), payload.end());
    }
};

// ─────────────────────────────────────────────────────────────
// Wire format:
//
//  ┌──────────┬──────────┬─────────────┬─────────────────────┐
//  │  1 byte  │  4 bytes │   4 bytes   │  payload_len bytes  │
//  │   type   │    id    │ payload_len │      payload        │
//  └──────────┴──────────┴─────────────┴─────────────────────┘
//  Total header = 9 bytes
// ─────────────────────────────────────────────────────────────
static constexpr size_t HEADER_SIZE = 9;  // 1 + 4 + 4

// Serialize a Message into bytes ready to send over TCP
inline std::vector<char> encode(const Message& msg) {
    uint32_t payload_len = static_cast<uint32_t>(msg.payload.size());

    std::vector<char> buf(HEADER_SIZE + payload_len);

    // type (1 byte)
    buf[0] = static_cast<char>(msg.type);

    // id (4 bytes, big-endian)
    uint32_t id_net = htonl(msg.id);
    std::memcpy(&buf[1], &id_net, 4);

    // payload_len (4 bytes, big-endian)
    uint32_t len_net = htonl(payload_len);
    std::memcpy(&buf[5], &len_net, 4);

    // payload
    if (payload_len > 0)
        std::memcpy(&buf[HEADER_SIZE], msg.payload.data(), payload_len);

    return buf;
}

// ─────────────────────────────────────────────────────────────
// Low-level socket I/O helpers
//
// send_all / recv_all ensure the full N bytes are transferred.
// TCP may deliver data in chunks — these loop until complete.
// This is called "exact I/O" and is required for any binary protocol.
// ─────────────────────────────────────────────────────────────
inline bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool recv_all(int fd, char* buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = ::recv(fd, buf + recvd, len - recvd, 0);
        if (n <= 0) return false;
        recvd += static_cast<size_t>(n);
    }
    return true;
}

// Send a Message over a socket
inline bool send_message(int fd, const Message& msg) {
    auto buf = encode(msg);
    return send_all(fd, buf.data(), buf.size());
}

// Receive a Message from a socket
// Returns false if connection closed or error
inline bool recv_message(int fd, Message& out) {
    char header[HEADER_SIZE];
    if (!recv_all(fd, header, HEADER_SIZE)) return false;

    // Parse type
    out.type = static_cast<MessageType>(header[0]);

    // Parse id
    uint32_t id_net;
    std::memcpy(&id_net, &header[1], 4);
    out.id = ntohl(id_net);

    // Parse payload length
    uint32_t len_net;
    std::memcpy(&len_net, &header[5], 4);
    uint32_t payload_len = ntohl(len_net);

    // Sanity check — reject absurdly large payloads (DoS protection)
    constexpr uint32_t MAX_PAYLOAD = 64 * 1024 * 1024;  // 64 MB
    if (payload_len > MAX_PAYLOAD)
        return false;

    // Read payload
    out.payload.resize(payload_len);
    if (payload_len > 0)
        if (!recv_all(fd, out.payload.data(), payload_len)) return false;

    return true;
}

} // namespace proto
