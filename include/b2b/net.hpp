// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "b2b/time.hpp"

namespace b2b {

// An IPv4 address and port. Kept platform-neutral (no OS socket headers here)
// so this header can be included anywhere, including on Windows.
struct Endpoint {
    uint32_t ipv4 = 0; // network byte order
    uint16_t port = 0; // host byte order
    bool valid = false;

    bool operator==(const Endpoint& o) const {
        return valid == o.valid && port == o.port && ipv4 == o.ipv4;
    }
    std::string toString() const;
};

// Minimal IPv4 UDP socket (Linux, macOS and Windows).
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool bind(uint16_t port); // port 0 = any free port
    uint16_t localPort() const;

    static bool resolve(const std::string& host, uint16_t port, Endpoint& out);

    // The IPv4 address this computer uses to reach the internet (its LAN
    // address behind a router). Sends nothing. False if offline.
    static bool primaryLocalIpv4(uint32_t& out);

    bool sendTo(const Endpoint& to, const uint8_t* data, size_t length);

    // Returns bytes received, 0 on timeout, -1 on error.
    int recvFrom(uint8_t* buffer, size_t capacity, Endpoint& from, int timeoutMs);

private:
    intptr_t fd_ = -1; // int on POSIX, SOCKET on Windows
};

// A reliable two-way byte stream (a TCP connection, or the session's own
// stream over UDP).
class ByteStream {
public:
    virtual ~ByteStream() = default;
    // Bytes transferred, 0 if it would block, -1 if the stream is gone.
    virtual int send(const uint8_t* data, size_t length) = 0;
    virtual int recv(uint8_t* buffer, size_t capacity) = 0;
    // Wait until readable and/or writable. Returns a mask (1 = readable,
    // 2 = writable), 0 on timeout, -1 on error.
    virtual int wait(bool wantRead, bool wantWrite, int timeoutMs) = 0;
};

// Minimal non-blocking IPv4 TCP socket (Linux, macOS and Windows).
class TcpSocket : public ByteStream {
public:
    TcpSocket() = default;
    ~TcpSocket() override;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool listen(uint16_t port);
    // Non-blocking: returns nullptr if nobody is waiting.
    std::unique_ptr<TcpSocket> accept();
    bool connect(const Endpoint& to, int timeoutMs);

    int send(const uint8_t* data, size_t length) override;
    int recv(uint8_t* buffer, size_t capacity) override;
    int wait(bool wantRead, bool wantWrite, int timeoutMs) override;

    bool valid() const { return fd_ != -1; }
    void close();

private:
    explicit TcpSocket(intptr_t fd) : fd_(fd) {}
    intptr_t fd_ = -1;
};

// Fake bad internet, applied to packets we send: base delay, random jitter and
// packet loss. Both peers apply it to their outgoing packets, so a round trip
// sees it twice, just like a real connection.
struct NetSimConfig {
    double delayMs = 0;   // fixed one-way delay
    double jitterMs = 0;  // extra random delay (exponential, mean = jitterMs)
    double lossPct = 0;   // percentage of packets dropped
    double bandwidthKBps = 0; // upload capacity with an unbounded queue in front
                              // (like a mobile link's big buffers); 0 = unlimited
    uint32_t seed = 1;

    bool enabled() const { return delayMs > 0 || jitterMs > 0 || lossPct > 0 || bandwidthKBps > 0; }
};

// Sends packets through the socket, optionally via the network simulator.
class Link {
public:
    Link(UdpSocket& socket, NetSimConfig sim);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    void send(const Endpoint& to, std::vector<uint8_t> packet);

private:
    struct Pending {
        Nanos deliverAt;
        uint64_t order;
        Endpoint to;
        std::vector<uint8_t> packet;
        bool operator>(const Pending& o) const {
            return deliverAt != o.deliverAt ? deliverAt > o.deliverAt : order > o.order;
        }
    };

    void run();

    UdpSocket& socket_;
    NetSimConfig sim_;
    std::mt19937 rng_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> queue_;
    uint64_t order_ = 0;
    Nanos linkFreeAt_ = 0; // bandwidth sim: when the queued packets have all gone out
    bool stop_ = false;
    std::thread thread_;
};

} // namespace b2b
