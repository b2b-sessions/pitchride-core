// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/net.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using SockLen = int;
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace b2b {

namespace {

#ifdef _WIN32
// Winsock must be started once per process.
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};
void ensureWinsock() {
    static WinsockInit init;
}
#else
void ensureWinsock() {}
#endif

NativeSocket native(intptr_t fd) { return static_cast<NativeSocket>(fd); }

sockaddr_in toSockaddr(const Endpoint& e) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = e.ipv4;
    addr.sin_port = htons(e.port);
    return addr;
}

} // namespace

std::string Endpoint::toString() const {
    if (!valid) {
        return "(none)";
    }
    char ip[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = ipv4;
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(port);
}

UdpSocket::~UdpSocket() {
    if (fd_ != static_cast<intptr_t>(kNoSocket)) {
#ifdef _WIN32
        closesocket(native(fd_));
#else
        ::close(native(fd_));
#endif
    }
}

bool UdpSocket::bind(uint16_t port) {
    ensureWinsock();
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        return false;
    }
    fd_ = static_cast<intptr_t>(s);
#ifdef _WIN32
    // Stop Windows reporting "connection reset" on a UDP socket when a packet
    // we sent earlier bounced (e.g. the leader was not running yet).
    BOOL reportReset = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
#endif
    // Room for bursts (a track streaming in alongside the sync traffic).
    // Windows defaults to 64 KB; the system may cap what we ask for.
    const int bufferBytes = 4 * 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::primaryLocalIpv4(uint32_t& out) {
    ensureWinsock();
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        return false;
    }
    // "Connecting" a UDP socket only picks a route; no packet is sent.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    sockaddr_in local{};
    SockLen len = sizeof(local);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
                    ::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
                    local.sin_addr.s_addr != 0;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    if (ok) {
        out = local.sin_addr.s_addr;
    }
    return ok;
}

uint16_t UdpSocket::localPort() const {
    sockaddr_in addr{};
    SockLen len = sizeof(addr);
    if (::getsockname(native(fd_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool UdpSocket::resolve(const std::string& host, uint16_t port, Endpoint& out) {
    ensureWinsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
#ifdef B2B_FALLBACK_HOST
        // Our server by name (connect.pitchride.co.uk) not found (its DNS
        // record, or a DNS problem here): its address, built in.
        if (host == B2B_DEFAULT_HOST && host != B2B_FALLBACK_HOST) {
            return resolve(B2B_FALLBACK_HOST, port, out);
        }
#endif
        return false;
    }
    sockaddr_in addr{};
    std::memcpy(&addr, result->ai_addr, sizeof(sockaddr_in));
    ::freeaddrinfo(result);
    out.ipv4 = addr.sin_addr.s_addr;
    out.port = port;
    out.valid = true;
    return true;
}

bool UdpSocket::sendTo(const Endpoint& to, const uint8_t* data, size_t length) {
    if (fd_ == static_cast<intptr_t>(kNoSocket) || !to.valid) {
        return false;
    }
    const sockaddr_in addr = toSockaddr(to);
    const auto sent = ::sendto(native(fd_), reinterpret_cast<const char*>(data), static_cast<int>(length), 0,
                               reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return static_cast<long long>(sent) == static_cast<long long>(length);
}

int UdpSocket::recvFrom(uint8_t* buffer, size_t capacity, Endpoint& from, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = native(fd_);
    pfd.events = POLLRDNORM;
    const int ready = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{native(fd_), POLLIN, 0};
    const int ready = ::poll(&pfd, 1, timeoutMs);
#endif
    if (ready <= 0) {
        return ready; // 0 = timeout, -1 = error
    }
    sockaddr_in addr{};
    SockLen len = sizeof(addr);
    const auto n = ::recvfrom(native(fd_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0,
                              reinterpret_cast<sockaddr*>(&addr), &len);
    if (n < 0) {
#ifdef _WIN32
        // A previous send to a closed port makes Windows report an error here; ignore it.
        if (WSAGetLastError() == WSAECONNRESET) {
            return 0;
        }
#endif
        return -1;
    }
    from.ipv4 = addr.sin_addr.s_addr;
    from.port = ntohs(addr.sin_port);
    from.valid = true;
    return static_cast<int>(n);
}

// ---------------------------------------------------------------------------
// TcpSocket

namespace {

void setNonBlocking(NativeSocket s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
}

void closeNative(NativeSocket s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

} // namespace

TcpSocket::~TcpSocket() { close(); }

void TcpSocket::close() {
    if (fd_ != -1) {
        closeNative(native(fd_));
        fd_ = -1;
    }
}

bool TcpSocket::listen(uint16_t port) {
    ensureWinsock();
    close();
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        return false;
    }
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 4) != 0) {
        closeNative(s);
        return false;
    }
    setNonBlocking(s);
    fd_ = static_cast<intptr_t>(s);
    return true;
}

std::unique_ptr<TcpSocket> TcpSocket::accept() {
    if (fd_ == -1) {
        return nullptr;
    }
    const NativeSocket c = ::accept(native(fd_), nullptr, nullptr);
    if (c == kNoSocket) {
        return nullptr;
    }
    setNonBlocking(c);
    int yes = 1;
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    return std::unique_ptr<TcpSocket>(new TcpSocket(static_cast<intptr_t>(c)));
}

bool TcpSocket::connect(const Endpoint& to, int timeoutMs) {
    ensureWinsock();
    close();
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        return false;
    }
    setNonBlocking(s);
    const sockaddr_in addr = toSockaddr(to);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 && !wouldBlock()) {
        closeNative(s);
        return false;
    }
    fd_ = static_cast<intptr_t>(s);
    if (wait(false, true, timeoutMs) != 2) {
        close();
        return false;
    }
    int err = 0;
    SockLen len = sizeof(err);
    ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0) {
        close();
        return false;
    }
    int yes = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    return true;
}

int TcpSocket::send(const uint8_t* data, size_t length) {
    if (fd_ == -1) {
        return -1;
    }
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const auto n = ::send(native(fd_), reinterpret_cast<const char*>(data), static_cast<int>(length), flags);
    if (n < 0) {
        return wouldBlock() ? 0 : -1;
    }
    return static_cast<int>(n);
}

int TcpSocket::recv(uint8_t* buffer, size_t capacity) {
    if (fd_ == -1) {
        return -1;
    }
    const auto n = ::recv(native(fd_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0);
    if (n == 0) {
        return -1; // closed by peer
    }
    if (n < 0) {
        return wouldBlock() ? 0 : -1;
    }
    return static_cast<int>(n);
}

int TcpSocket::wait(bool wantRead, bool wantWrite, int timeoutMs) {
    if (fd_ == -1) {
        return -1;
    }
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = native(fd_);
    pfd.events = static_cast<SHORT>((wantRead ? POLLRDNORM : 0) | (wantWrite ? POLLWRNORM : 0));
    const int ready = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{native(fd_), static_cast<short>((wantRead ? POLLIN : 0) | (wantWrite ? POLLOUT : 0)), 0};
    const int ready = ::poll(&pfd, 1, timeoutMs);
#endif
    if (ready < 0) {
        return -1;
    }
    if (ready == 0) {
        return 0;
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) {
        return -1;
    }
    int mask = 0;
    if (pfd.revents & (POLLIN | POLLHUP)) {
        mask |= 1;
    }
    if (pfd.revents & POLLOUT) {
        mask |= 2;
    }
    return mask;
}

// ---------------------------------------------------------------------------
// Link

Link::Link(UdpSocket& socket, NetSimConfig sim) : socket_(socket), sim_(sim), rng_(sim.seed) {
    if (sim_.enabled()) {
        thread_ = std::thread([this] { run(); });
    }
}

Link::~Link() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Link::send(const Endpoint& to, std::vector<uint8_t> packet) {
    if (!sim_.enabled()) {
        socket_.sendTo(to, packet.data(), packet.size());
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::uniform_real_distribution<double> uniform(0.0, 100.0);
    if (uniform(rng_) < sim_.lossPct) {
        return; // dropped
    }
    double delayMs = sim_.delayMs;
    if (sim_.jitterMs > 0) {
        std::exponential_distribution<double> jitter(1.0 / sim_.jitterMs);
        delayMs += jitter(rng_);
    }
    Nanos sendAt = trueNow();
    if (sim_.bandwidthKBps > 0) {
        // Waits behind everything already queued, then takes its own time on the wire.
        sendAt = std::max(sendAt, linkFreeAt_) +
                 static_cast<Nanos>(static_cast<double>(packet.size()) / (sim_.bandwidthKBps * 1024.0) * 1e9);
        linkFreeAt_ = sendAt;
    }
    queue_.push({sendAt + fromMs(delayMs), order_++, to, std::move(packet)});
    cv_.notify_all();
}

void Link::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) {
            cv_.wait(lock);
            continue;
        }
        const Nanos wait = queue_.top().deliverAt - trueNow();
        if (wait > 0) {
            cv_.wait_for(lock, std::chrono::nanoseconds(wait));
            continue;
        }
        Pending item = queue_.top();
        queue_.pop();
        lock.unlock();
        socket_.sendTo(item.to, item.packet.data(), item.packet.size());
        lock.lock();
    }
}

} // namespace b2b
