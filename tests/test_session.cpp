// SPDX-License-Identifier: GPL-2.0-or-later
//
// Encrypted sessions connected by address (no server): passwords, strangers,
// and telling the user when versions differ.

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#include "b2b/protocol.hpp"
#include "b2b/session.hpp"

using namespace b2b;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

bool waitFor(const std::function<bool()>& done, int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

SessionConfig hostConfig(uint16_t port, const std::string& password) {
    SessionConfig c;
    c.role = Role::Leader;
    c.localPort = port;
    c.password = password;
    c.startInNs = 300 * kNanosPerMs;
    return c;
}

SessionConfig guestConfig(uint16_t port, const std::string& password) {
    SessionConfig c;
    c.role = Role::Follower;
    c.peerHost = "127.0.0.1";
    c.peerPort = port;
    c.password = password;
    return c;
}

bool sendsEvent(Session& from, Session& to, uint32_t seq) {
    ControlEvent ev;
    ev.seq = seq;
    ev.time = from.sharedNow();
    ev.key.assign("[Channel1],volume");
    ev.value = seq;
    from.sendEvent(ev);
    ControlEvent got;
    return waitFor([&] { return to.popReceivedEvent(got) && got.value == seq; }, 2000);
}

void testPassword() {
    std::puts("right password connects; typed loosely is fine");
    LocalClock hc, gc(4 * kNanosPerSec);
    Session host(hostConfig(9581, "KX7P-Q2MD-9TZA"), hc);
    Session guest(guestConfig(9581, "kx7p q2md 9tza"), gc);
    CHECK(host.start() && guest.start());
    CHECK(waitFor([&] { return guest.ready() && host.peerGeneration() > 0; }, 4000));
    CHECK(host.encrypted() && guest.encrypted());
    CHECK(sendsEvent(host, guest, 1));
    CHECK(sendsEvent(guest, host, 2));
    CHECK(host.linkProblem() == LinkProblem::None);
    CHECK(guest.peerAppVersion() == kVersion);

    std::puts("a stranger with the wrong password can't join or disturb the session");
    LocalClock sc;
    Session stranger(guestConfig(9581, "WRONG-WRONG-WRONG"), sc);
    CHECK(stranger.start());
    CHECK(waitFor([&] { return stranger.linkProblem() == LinkProblem::WrongPassword; }, 6000));
    CHECK(!stranger.ready());
    CHECK(sendsEvent(host, guest, 3)); // the real guest is still the peer
    CHECK(sendsEvent(guest, host, 4));
    CHECK(host.peerAddress() == "127.0.0.1:" + std::to_string(guest.localPort()));
    stranger.stop();
    guest.stop();
    host.stop();
}

void testWrongPassword() {
    std::puts("wrong password: nobody connects, both sides are told");
    LocalClock hc, gc;
    Session host(hostConfig(9582, "AAAA-BBBB-CCCC"), hc);
    Session guest(guestConfig(9582, "AAAA-BBBB-CCCD"), gc);
    CHECK(host.start() && guest.start());
    CHECK(waitFor([&] { return guest.linkProblem() == LinkProblem::WrongPassword; }, 6000));
    CHECK(waitFor([&] { return host.linkProblem() == LinkProblem::WrongPassword; }, 6000));
    CHECK(!guest.ready() && host.peerGeneration() == 0);
    guest.stop();
    host.stop();
}

void testVersions() {
    std::puts("an older app (unencrypted protocol) or a newer one is recognised");
    LocalClock hc;
    Session host(hostConfig(9583, "X"), hc);
    CHECK(host.start());
    Endpoint hostEp;
    CHECK(UdpSocket::resolve("127.0.0.1", 9583, hostEp));
    UdpSocket old;
    CHECK(old.bind(0));
    // What a 0.3 app sends first: a plain B2B3 ping.
    std::vector<uint8_t> ping = {0x42, 0x32, 0x42, 0x33, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    old.sendTo(hostEp, ping.data(), ping.size());
    CHECK(waitFor([&] { return host.linkProblem() == LinkProblem::PeerOutdated; }, 2000));
    host.stop();

    Session host2(hostConfig(9584, "X"), hc);
    CHECK(host2.start());
    CHECK(UdpSocket::resolve("127.0.0.1", 9584, hostEp));
    HelloMsg future;
    future.role = 1;
    future.protocol = kProtocolVersion + 1;
    future.appVersion = "9.9.9";
    const auto bytes = encodeHello(future);
    old.sendTo(hostEp, bytes.data(), bytes.size());
    CHECK(waitFor([&] { return host2.linkProblem() == LinkProblem::PeerNewer; }, 2000));
    CHECK(host2.peerAppVersion() == "9.9.9");
    host2.stop();
}

} // namespace

int main() {
    testPassword();
    testWrongPassword();
    testVersions();
    if (gFailures == 0) {
        std::puts("all session tests passed");
        return 0;
    }
    std::printf("%d failure(s)\n", gFailures);
    return 1;
}
